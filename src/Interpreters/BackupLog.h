#pragma once

#include "Common/Exception.h"
#include <Interpreters/SystemLog.h>
#include <Backups/BackupStatus.h>
#include <Core/NamesAndTypes.h>
#include <Core/NamesAndAliases.h>
#include <Parsers/IAST_fwd.h>
#include <Storages/SelectQueryInfo.h>


namespace DB
{

struct BackupLogElement
{
    String id;
    BackupStatus status;
    String error;
    time_t start_time;
    time_t end_time;
    UInt64 num_files;
    UInt64 total_size;
    UInt64 num_entries;
    UInt64 uncompressed_size;
    UInt64 compressed_size;
    UInt64 files_read;
    UInt64 bytes_read;

    static std::string name() { return "BackupLog"; }
    static NamesAndTypesList getNamesAndTypes();
    static NamesAndAliases getNamesAndAliases() { return {}; }
    void appendToBlock(MutableColumns & res_columns, ContextPtr context, const SelectQueryInfo &) const;
    static const char * getCustomColumnList() { return nullptr; }
};

class BackupLog : public SystemLog<BackupLogElement>
{
public:
    using SystemLog<BackupLogElement>::SystemLog;

    /// This table is usually queried for fixed table name.
    static const char * getDefaultOrderBy() { return "start_time, end_time"; }
};

}
