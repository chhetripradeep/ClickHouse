#include <TableFunctions/TableFunctionPostgreSQL.h>

#if USE_LIBPQXX
#include <Interpreters/evaluateConstantExpression.h>
#include <Parsers/ASTFunction.h>
#include <TableFunctions/ITableFunction.h>
#include <TableFunctions/TableFunctionFactory.h>
#include <Common/Exception.h>
#include "registerTableFunctions.h"
#include <Common/parseRemoteDescription.h>


namespace DB
{

namespace ErrorCodes
{
    extern const int BAD_ARGUMENTS;
}


StoragePtr TableFunctionPostgreSQL::executeImpl(const ASTPtr & /*ast_function*/,
        ContextPtr context, const std::string & table_name, ColumnsDescription cached_columns, bool /*is_insert_query*/) const
{
    auto result = std::make_shared<StoragePostgreSQL>(
        StorageID(getDatabaseName(), table_name),
        connection_pool,
        configuration->table,
        cached_columns,
        ConstraintsDescription{},
        String{},
        context,
        configuration->schema,
        configuration->on_conflict);

    result->startup();
    return result;
}


ColumnsDescription TableFunctionPostgreSQL::getActualTableStructure(ContextPtr context, bool /*is_insert_query*/) const
{
    return StoragePostgreSQL::getTableStructureFromData(connection_pool, configuration->table, configuration->schema, context);
}


void TableFunctionPostgreSQL::parseArguments(const ASTPtr & ast_function, ContextPtr context)
{
    const auto & func_args = ast_function->as<ASTFunction &>();

    if (!func_args.arguments)
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "Table function 'PostgreSQL' must have arguments.");

    auto & args = func_args.arguments->children;

    PostgreSQLSettings postgresql_settings;

    const auto & settings = context->getSettingsRef();
    postgresql_settings.connect_timeout = settings.external_storage_connect_timeout_sec;
    postgresql_settings.read_write_timeout = settings.external_storage_rw_timeout_sec;

    for (auto * it = args.begin(); it != args.end(); ++it)
    {
        const ASTSetQuery * settings_ast = (*it)->as<ASTSetQuery>();
        if (settings_ast)
        {
            postgresql_settings.loadFromQuery(*settings_ast);
            args.erase(it);
            break;
        }
    }

    configuration = StoragePostgreSQL::getConfiguration(args, context, postgresql_settings);
    connection_pool = std::make_shared<postgres::PoolWithFailover>(
        *configuration,
        settings.postgresql_connection_pool_size,
        settings.postgresql_connection_pool_wait_timeout,
        POSTGRESQL_POOL_WITH_FAILOVER_DEFAULT_MAX_TRIES,
        settings.postgresql_connection_pool_auto_close_connection);
}


void registerTableFunctionPostgreSQL(TableFunctionFactory & factory)
{
    factory.registerFunction<TableFunctionPostgreSQL>();
}

}

#endif
