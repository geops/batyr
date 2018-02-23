#include <sstream>
#include <cstring>
#include <algorithm>

#include "server/db/transaction.h"
#include "server/db/connection.h"
#include "common/macros.h"
#include "common/config.h"
#include "common/stringutils.h"


using namespace Batyr::Db;


Transaction::Transaction(Connection * _connection)
    :   logger(Poco::Logger::get("Db::Transaction")),
        connection(_connection),
        rollback(false)
{
    poco_debug(logger, "BEGIN");

    // start transaction
    exec("begin;");
}


Transaction::~Transaction()
{
    auto transactionStatus = PQtransactionStatus(connection->pgconn);
    if (rollback || (transactionStatus == PQTRANS_INERROR) || (transactionStatus == PQTRANS_UNKNOWN)) {
        poco_debug(logger, "ROLLBACK");
        exec("rollback;");
    }
    else {
        poco_debug(logger, "COMMIT");
        exec("commit;");
    }

    // run all exitSqls
    for(auto &exitSql: exitSqls) {
        try {
            poco_debug(logger, "Running Exit SQL: " + exitSql);
            exec(exitSql);
        }
        catch(DbError &e) {
            poco_warning(logger, "Transaction Exit SQL failed: "+exitSql);
        }
    }
    exitSqls.clear();
}


PGresultPtr
Transaction::exec(const std::string &_sql)
{
    PGresultPtr result( PQexec(connection->pgconn, _sql.c_str()), PQclear);
    checkResult(result);
    return std::move(result);
}


PGresultPtr
Transaction::execParams(const std::string &_sql, int nParams, const Oid *paramTypes,
            const char * const *paramValues, const int *paramLengths, const int *paramFormats, int resultFormat)
{
    PGresultPtr result( PQexecParams(connection->pgconn, _sql.c_str(),
                nParams,
                paramTypes,
                paramValues,
                paramLengths,
                paramFormats,
                resultFormat
            ), PQclear);
    checkResult(result);
    return std::move(result);
}


PGresultPtr
Transaction::prepare(const std::string &stmtName, const std::string &_sql, int nParams, const Oid *paramTypes)
{
    PGresultPtr result( PQprepare(connection->pgconn, stmtName.c_str(), _sql.c_str(),
                nParams,
                paramTypes
            ), PQclear);
    checkResult(result);

    // explicitly deallocate the prepared statement at the end of the transaction. otherwise
    // the statement stays allocated until the end of the db session
    std::stringstream deallocStmtStream;
    deallocStmtStream << "deallocate " << stmtName << ";";
    exitSqls.push_back(deallocStmtStream.str());

    return std::move(result);
}


PGresultPtr
Transaction::execPrepared(const std::string &stmtName, int nParams, const char * const *paramValues, const int *paramLengths,
                         const int *paramFormats, int resultFormat)
{
    PGresultPtr result( PQexecPrepared(connection->pgconn, stmtName.c_str(),
                nParams,
                paramValues,
                paramLengths,
                paramFormats,
                resultFormat
            ),PQclear);
    checkResult(result);
    return std::move(result);
}


PGresultPtr
Transaction::execPrepared(const std::string &stmtName, const std::vector<QueryValue> &qValues)
{
    // convert to an array of c strings
    auto params = PGParams(qValues);

    PGresultPtr result = this->execPrepared(stmtName, params.length(), params.values(), params.valueLenghts(), NULL, 1);

    return std::move(result);
}


PGresultPtr
Transaction::execParams(const std::string &_sql, const std::vector<QueryValue> &qValues)
{
    // convert to an array of c strings
    auto params = PGParams(qValues);

    PGresultPtr result = this->execParams(_sql, params.length(), NULL, params.values(), params.valueLenghts(), NULL, 1);

    return std::move(result);
}

void
Transaction::checkResult(PGresultPtr & res)
{
    if (!res) {
        std::string msg = "query result was null: " + std::string(PQerrorMessage(connection->pgconn));
        poco_error(logger, msg.c_str());
        rollback = true;
        throw DbError(msg);
    }

    auto resStatus = PQresultStatus(res.get());
    if (resStatus == PGRES_FATAL_ERROR) {
        char * sqlstate = PQresultErrorField(res.get(), PG_DIAG_SQLSTATE);
        char * msg_primary = PQresultErrorField(res.get(), PG_DIAG_MESSAGE_PRIMARY);
        char * context = PQresultErrorField(res.get(), PG_DIAG_CONTEXT);

#ifdef _DEBUG
        std::stringstream msgstream;
        msgstream << "query failed: " << msg_primary << " [sqlstate: " << sqlstate << "]";
        poco_debug(logger, msgstream.str().c_str());
#endif

        rollback = true;

        auto excep = DbError(msg_primary, sqlstate);
        if (context != nullptr) {
            excep.setContext(context);
        }
        throw excep;
    }
}

void
Transaction::createTempTable(const std::string &existingTableSchema, const std::string &existingTableName, const std::string &tempTableName)
{

    poco_debug(logger, "creating table " + tempTableName + " based on " + existingTableName);
    std::string qTempTableName = quoteIdent(tempTableName);

    std::stringstream querystream;
    querystream << "create temporary";

    // temporary tables will anyways be gone after a server crash so
    // we can also benefit from unlogged tables and skip the WAL
    if (connection->getVersion() >= 901000) {
        querystream << " unlogged";
    }

    // use a simple select into and not table inheritance
    querystream << " table " << qTempTableName << " as"
                << " select * from"
                << " " << quoteAndJoinIdent(existingTableSchema, existingTableName)
                << " limit 0";
    std::string query = querystream.str();

    // postgresql drops temporary tables when the connections ends. But the temporary
    // tables are not needed anymore when the transaction ends. ensuring that they
    // are dropped immetiately after the end of the transaction
    std::stringstream dropTableStream;
    dropTableStream << "drop table if exists " << qTempTableName;
    exitSqls.push_back(dropTableStream.str());

    // create the temporary table
    exec(query);
}


FieldMap
Transaction::getTableFields(const std::string &tableSchema, const std::string &tableName)
{
    FieldMap fieldMap;

    const char *paramValues[2] = { 
            tableName.c_str(),
            tableSchema.c_str()
    };
    int paramLengths[COUNT_OF(paramValues)] = {
            static_cast<int>(tableName.length()),
            static_cast<int>(tableSchema.length())
    };
    auto res = execParams(
                "select pa.attname, pt.typname, pt.oid, coalesce(is_pk.is_pk, 'N')::text as is_pk"
                " from pg_catalog.pg_attribute pa"
                " join pg_catalog.pg_class pc on pc.oid=pa.attrelid and pa.attnum>0"
                " join pg_catalog.pg_namespace pns on pc.relnamespace = pns.oid"
                " join pg_catalog.pg_type pt on pt.oid=pa.atttypid"
                " left join ("
                "    select pcs.conrelid as relid, unnest(conkey) as attnum, 'Y'::text as is_pk "
                "        from pg_catalog.pg_constraint pcs"
                "        where pcs.contype = 'p'"
                " ) is_pk on is_pk.relid = pc.oid and is_pk.attnum=pa.attnum"
                " where pc.relname = $1::text and pns.nspname = $2::text",
            COUNT_OF(paramValues), NULL, paramValues, paramLengths, NULL, 1);

    for (int i=0; i < PQntuples(res.get()); i++) {
        char * attname = PQgetvalue(res.get(), i, 0);
        char * typname = PQgetvalue(res.get(), i, 1);
        char * oid = PQgetvalue(res.get(), i, 2);
        char * isPk = PQgetvalue(res.get(), i, 3);

        auto field = &fieldMap[attname];
        if (!field->name.empty()) {
            throw DbError("Tables with multiple columns with the same name are not supported. Column: "+std::string(attname));
        }

        field->name = attname;
        field->pgTypeName = typname;
        field->pgTypeOid = std::atoi(oid);
        field->isPrimaryKey = (std::strcmp(isPk,"Y") == 0);
    }
    return std::move(fieldMap);
}


std::vector<std::string> 
Transaction::quoteIdent(const std::vector<std::string> & inStrings)
{
    std::vector<std::string> quotedStrings;
    size_t inStringsSize = inStrings.size();

    if (inStringsSize > 0) {
#ifdef HAVE_PQ_ESCAPE_IDENTIFIER
        for(auto &inString: inStrings) {
            char * escaped = PQescapeIdentifier(connection->pgconn, inString.c_str(), 
                        inString.length());
            if (escaped != nullptr) {
                quotedStrings.push_back(std::string(escaped));
                PQfreemem(escaped);
            }
            else {
                std::string msg = "quoting sql identifier <" + inString 
                            + "> failed: " + std::string(PQerrorMessage(connection->pgconn));
                poco_error(logger, msg.c_str());
                throw DbError(msg);
            }
        }
#else
        std::stringstream queryStrm;

        size_t iParam = 0;
        std::vector<const char *> paramValues;
        paramValues.reserve(inStringsSize);
        std::vector<int> paramLengths;
        paramLengths.reserve(inStringsSize);

        queryStrm << "select ";
        while (iParam < inStringsSize) {
            if (iParam > 0) {
                queryStrm << ", ";
            }
            queryStrm << "quote_ident($" << iParam+1 << ")";
            paramValues.push_back(inStrings[iParam].c_str());
            paramLengths.push_back(inStrings[iParam].length());
            ++iParam;
        }

        auto res = execParams(
                queryStrm.str(),
                paramValues.size(), NULL, &paramValues[0], &paramLengths[0], NULL, 1);

        if (PQntuples(res.get()) != 1) {
            throw DbError("Transaction::quoteIdent should only get one tuple from db");
        }

        for (size_t i=0; i < inStringsSize; i++) {
            char * val = PQgetvalue(res.get(), 0, i);
            quotedStrings.push_back(std::string(val));
        }
#endif
    }
    return quotedStrings;
}

        
std::string 
Transaction::quoteIdent(const std::string & uq1)
{
    std::vector<std::string> uqv;
    uqv.push_back(uq1);
    
    auto qv = quoteIdent(uqv);
    if (qv.size() != 1) {
        throw DbError("Quoted vector should contain excatly one element.");
    }
    return qv[0];
}


std::string
Transaction::quoteAndJoinIdent(const std::string & uq1, const std::string & uq2)
{
    std::vector<std::string> uqv;
    uqv.push_back(uq1);
    uqv.push_back(uq2);
    
    auto qv = quoteIdent(uqv);
    if (qv.size() != 2) {
        throw DbError("Quoted vector should contain exactly two elements.");
    }
    return StringUtils::join(qv, ".");
}


PGParams::PGParams(const std::vector<QueryValue> &qValues) 
    :   _values(NULL),
        _valueLengths(NULL),
        _length(0)
{
    _length = qValues.size();
    _values = new char *[_length];
    _valueLengths = new int [_length];

    int i = 0;
    for (auto const &pV: qValues) {
        if (pV.isNull()) {
            _values[i] = NULL;
            _valueLengths[i] = 0;
        }
        else {
            auto v = pV.get();
            _values[i] = strdup(v.c_str());
            _valueLengths[i] = static_cast<int>(v.length());
        }
        i++;
    }
}

PGParams::~PGParams()
{
    if (_values != NULL) {
        for(int i = 0; i<_length; i++) {
            if (_values[i] != NULL) {
                free(_values[i]);
            }
        }
        delete[] _values;
    }
    if (_valueLengths != NULL) {
        delete[] _valueLengths;
    }
}
