/* Copyright (c) 2018 BlackBerry Limited

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at
http://www.apache.org/licenses/LICENSE-2.0
Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. */
#include <Core/Settings.h>
#include <Common/typeid_cast.h>
#include <Parsers/ASTWatchQuery.h>
#include <Interpreters/InterpreterWatchQuery.h>
#include <DataStreams/IBlockInputStream.h>
#include <DataStreams/OneBlockInputStream.h>


namespace DB
{

namespace ErrorCodes
{
    extern const int UNKNOWN_STORAGE;
    extern const int UNKNOWN_TABLE;
    extern const int TOO_MANY_COLUMNS;
    extern const int SUPPORT_IS_DISABLED;
}

BlockInputStreamPtr InterpreterWatchQuery::executeImpl()
{
    return std::make_shared<OneBlockInputStream>(Block());
}

BlockIO InterpreterWatchQuery::execute()
{
    if (!context.getSettingsRef().allow_experimental_live_view)
        throw Exception("Experimental LIVE VIEW feature is not enabled (the setting 'allow_experimental_live_view')", ErrorCodes::SUPPORT_IS_DISABLED);

    BlockIO res;
    const ASTWatchQuery & query = typeid_cast<const ASTWatchQuery &>(*query_ptr);
    String database;
    String table;
    /// Get database
    if (!query.database.empty())
        database = query.database;
    else
        database = context.getCurrentDatabase();

    /// Get table
    table = query.table;

    /// Get storage
    storage = context.tryGetTable(database, table);

    if (!storage)
        throw Exception("Table " + backQuoteIfNeed(database) + "." +
        backQuoteIfNeed(table) + " doesn't exist.",
        ErrorCodes::UNKNOWN_TABLE);

    /// List of columns to read to execute the query.
    Names required_columns = storage->getColumns().getNamesOfPhysical();

    /// Get context settings for this query
    const Settings & settings = context.getSettingsRef();

    /// Limitation on the number of columns to read.
    if (settings.max_columns_to_read && required_columns.size() > settings.max_columns_to_read)
        throw Exception("Limit for number of columns to read exceeded. "
            "Requested: " + std::to_string(required_columns.size())
            + ", maximum: " + settings.max_columns_to_read.toString(),
            ErrorCodes::TOO_MANY_COLUMNS);

    size_t max_block_size = settings.max_block_size;
    size_t max_streams = 1;

    /// Define query info
    SelectQueryInfo query_info;
    query_info.query = query_ptr;

    /// From stage
    QueryProcessingStage::Enum from_stage = QueryProcessingStage::FetchColumns;
    QueryProcessingStage::Enum to_stage = QueryProcessingStage::Complete;

    /// Watch storage
    streams = storage->watch(required_columns, query_info, context, from_stage, max_block_size, max_streams);

    /// Constraints on the result, the quota on the result, and also callback for progress.
    if (IBlockInputStream * stream = dynamic_cast<IBlockInputStream *>(streams[0].get()))
    {
        /// Constraints apply only to the final result.
        if (to_stage == QueryProcessingStage::Complete)
        {
            IBlockInputStream::LocalLimits limits;
            limits.mode = IBlockInputStream::LIMITS_CURRENT;
            limits.size_limits.max_rows = settings.max_result_rows;
            limits.size_limits.max_bytes = settings.max_result_bytes;
            limits.size_limits.overflow_mode = settings.result_overflow_mode;

            stream->setLimits(limits);
            stream->setQuota(context.getQuota());
        }
    }

    res.in = streams[0];

    return res;
}


}
