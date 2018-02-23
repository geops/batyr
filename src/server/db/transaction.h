#ifndef __batyr_db_transaction_h__
#define __batyr_db_transaction_h__

#include <Poco/Logger.h>

#include <libpq-fe.h>

#include "server/db/field.h"
#include "server/db/queryvalue.h"

#include <memory>
#include <vector>


namespace Batyr 
{
namespace Db
{
 
    class Connection; // forward decl;

    /** 
     * smartpointer to free a PGresult on scope exit
     */
    typedef std::unique_ptr<PGresult, void (*)(PGresult*) > PGresultPtr;


    class Transaction
    {
        private:
            Poco::Logger & logger;
            Connection * connection;

            /**
             * sqls which are executed when the transaction ends.
             * these commands will be send to the db regardless
             * if the transaction failed or was successful
             */
            std::vector< std::string > exitSqls;

            Transaction(Connection *);

            /**
             * discard/rollback all changes made in this transaction
             * regardless if an error occured or not
             */
            bool rollback;

            /**
             * check a PQresult if it was a scuccessfull
             * or throw a meaningful DbError
             **/
            void checkResult(PGresultPtr & res);
            

        public:
            friend class Connection;

            ~Transaction();

            /**
             * execute a sql query
             */
            PGresultPtr exec(const std::string &);
            PGresultPtr execParams(const std::string &_sql, int nParams, const Oid *paramTypes,
                        const char * const *paramValues, const int *paramLengths, const int *paramFormats, int resultFormat);
            PGresultPtr execParams(const std::string &_sql, const std::vector<QueryValue> &qValues);
            PGresultPtr prepare(const std::string &stmtName, const std::string &_sql, int nParams, const Oid *paramTypes);
            PGresultPtr execPrepared(const std::string &stmtName, int nParams, const char * const *paramValues, const int *paramLengths,
                        const int *paramFormats, int resultFormat);
            PGresultPtr execPrepared(const std::string &stmtName, const std::vector<QueryValue> &qValues);


            void discard()
            {
                rollback = true;
            }

            /**
             * Create a temporary table based upon the schema
             * of an existing table;
             *
             * The table will be dropped when the Transsction object is destroyed.
             */
            void createTempTable(const std::string &existingTableSchema, const std::string &existingTableName, const std::string &tempTableName); 

            /**
             * Return a FieldMap describing the columns of the given table.
             */
            FieldMap getTableFields(const std::string &tableSchema, const std::string &tableName);

            /** 
             * quote all strings of the vector as identifiers using 
             * postgresqls quote_ident function
             */
            std::vector<std::string> quoteIdent(const std::vector<std::string> &);
            std::string quoteIdent(const std::string &);

            /**
             * return the value quoted with quote_ident and all parts joined together with a '.'
             */
            std::string quoteAndJoinIdent(const std::string &, const std::string &);

    };


    /** RAI helper to create C arrays with parameters for libpq functions */
    class PGParams 
    {
        private:
            char ** _values;
            int * _valueLengths;
            int _length;

        public:
            PGParams(const std::vector<QueryValue> &qValues);
            ~PGParams();

            char ** values() 
            {
                return _values;
            };

            int * valueLenghts() 
            {
                return _valueLengths;
            };

            int length()
            {
                return _length;
            };
    };
};
};

#endif // __batyr_db_transaction_h__
