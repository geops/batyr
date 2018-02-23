#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <sstream>
#include <thread>
#include <vector>

#include "common/config.h"
#include "common/stringutils.h"
#include "server/worker.h"
#include "server/db/postgis.h"

using namespace Batyr;

struct OgrField
{
    std::string name;
    unsigned int index;
    OGRFieldType type;
};
typedef std::map<std::string, OgrField> OgrFieldMap;


Worker::Worker(Configuration::Ptr _configuration, std::shared_ptr<JobStorage> _jobs)
    :   logger(Poco::Logger::get("Worker")),
        configuration(_configuration),
        jobs(_jobs),
        db(_configuration)
{
    poco_debug(logger, "Creating Worker");
}


Worker::~Worker()
{
    poco_debug(logger, "Destroying Worker");
}


void
Worker::pull(Job::Ptr job)
{
    auto layer = configuration->getLayer(job->getLayerName());
    bool allow_feature_deletion = layer->allow_feature_deletion;

    // assemble the complete filter to filter the features od the source by
    std::string filterString;
    {
        std::stringstream filterStream;
        if ( (!layer->filter.empty()) && (!job->getFilter().empty()) ) {
            filterStream << "(";
        }
        if (!layer->filter.empty()) {
            filterStream << layer->filter;
        }
        if ( (!layer->filter.empty()) && (!job->getFilter().empty()) ) {
            filterStream << ") and (";
        }
        if (!job->getFilter().empty()) {
            filterStream << job->getFilter();
        }
        if ( (!layer->filter.empty()) && (!job->getFilter().empty()) ) {
            filterStream << ")";
        }
        filterString = filterStream.str();
    }

    // log the job
    {
        std::stringstream initialLogMsgStream;
        initialLogMsgStream     << "job " << job->getId()
                                << " pulling layer \"" << job->getLayerName() << "\"";
        if (!filterString.empty()) {
            initialLogMsgStream << " using filter \"" + filterString + "\"";
        }
        poco_information(logger, initialLogMsgStream.str().c_str());
    }

    // open the dataset
#if GDAL_VERSION_MAJOR > 1
    std::unique_ptr<GDALDataset> gdalDataset((GDALDataset*) GDALOpenEx(layer->source.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY, NULL, NULL, NULL));
#else
    std::unique_ptr<OGRDataSource, decltype((OGRDataSource::DestroyDataSource))> gdalDataset(
        OGRSFDriverRegistrar::Open(layer->source.c_str(), false), OGRDataSource::DestroyDataSource);
#endif
    if (!gdalDataset) {
        throw WorkerError("Could not open dataset for layer \"" + layer->name + "\"");
    }

    // find the layer
    auto ogrLayer = gdalDataset->GetLayerByName(layer->source_layer.c_str());
    if (ogrLayer == nullptr) {
        // check if the layer exists and is just not readable
        // and collect some info for better diagnosis of the
        // problem.
        bool layerExists = false;
        int layersNotOpenable = 0;
        std::string closestMatch;
        int ldClosestMatch = 5000; // initialize to a high value
        for (int layerIdx=0; layerIdx<gdalDataset->GetLayerCount(); layerIdx++) {
            auto searchLayer = gdalDataset->GetLayer(layerIdx);
            if (searchLayer != nullptr) {
                if (layer->source_layer == searchLayer->GetName()) {
                    layerExists = true;
                    break;
                }

                int ldSearchLayer = StringUtils::levenshteinDistance(
                            layer->source_layer, searchLayer->GetName());
                if (ldSearchLayer < ldClosestMatch) {
                    ldClosestMatch = ldSearchLayer;
                    closestMatch = searchLayer->GetName();
                }
            }
            else {
                layersNotOpenable++;
            }
        }

        std::stringstream msgstream;
        msgstream  << "source_layer \"" << layer->source_layer
                   << "\" for configured layer \"" << layer->name << "\" ";
        if (layerExists) {
            msgstream << "exists, but could not be opened.";
        }
        else {
            msgstream << "does not exist.";

            // info about next, best match if levensthein seems to be realistic to be
            // the layer we are looking for. Should help to debug typos, ...
            if (ldClosestMatch < 12) {
                msgstream   << " HINT: The source has a layer named \""
                            << closestMatch << "\".";
            }

            if (layersNotOpenable > 0) {
                msgstream   << " HINT: " << layersNotOpenable
                            << " layer(s) could not be opened.";
            }
        }

        throw WorkerError(msgstream.str());
    }
    ogrLayer->ResetReading();

    // set filter if there is one specified
    if (!filterString.empty()) {

        // when a filter is set the deletion of features is always disabled to allow
        // partial syncs without losing the rest of the data
        allow_feature_deletion = false;

        CPLErrorReset();
        if (ogrLayer->SetAttributeFilter(filterString.c_str()) != OGRERR_NONE) {
            std::stringstream msgstream;
            msgstream   << "The given filter for layer \""
                        << layer->name
                        << "\" is invalid";
            if (CPLGetLastErrorMsg()) {
                msgstream   << ": " << CPLGetLastErrorMsg();
            }
            else {
                msgstream   << ".";
            }
            msgstream   << " The applied filter was [ "
                        << filterString
                        << " ]";
            CPLErrorReset();
            throw WorkerError(msgstream.str());
        }
    }

    auto ogrFeatureDefn = ogrLayer->GetLayerDefn();

#if GDAL_VERSION_MAJOR > 1
    if (ogrFeatureDefn->GetGeomFieldCount() > 1) {
        std::string msg = "The source provides " + std::to_string(ogrFeatureDefn->GetGeomFieldCount()) +
                "geometry fields. Currently only sources with no or only one geometry field are supported";
        throw WorkerError(msg);
    }
#endif


#ifdef _DEBUG
        {
            if (strlen(ogrLayer->GetFIDColumn()) > 0) {
                std::stringstream msg;
                msg << "ogr layer provides the column "
                    << ogrLayer->GetFIDColumn()
                    << " (via fid)";
                poco_debug(logger, msg.str());
            }
        }
#endif

    // collect the columns of the dataset
    OgrFieldMap ogrFields;
    for(int i=0; i<ogrFeatureDefn->GetFieldCount(); i++) {
        auto ogrFieldDefn = ogrFeatureDefn->GetFieldDefn(i);

        // lowercase column names -- TODO: this may cause problems when postgresqls column names
        // contain uppercase letters, but will do for a start
        std::string fieldName = StringUtils::tolower(std::string(ogrFieldDefn->GetNameRef()));

#ifdef _DEBUG
        {
            std::string msg = "ogr layer provides the column " + fieldName;
            poco_debug(logger, msg.c_str());
        }
#endif
        auto entry = &ogrFields[fieldName];
        entry->index = i;
        entry->type = ogrFieldDefn->GetType();
    }

    // perform the work in an transaction
    if (auto transaction = db.getTransaction()) {
        int numPulled = 0;
        int numCreated = 0;
        int numUpdated = 0;
        int numDeleted = 0;
        int numIgnored = 0;

        // set the postgresql date style
        transaction->exec("set DateStyle to SQL, YMD");

        // build a unique name for the temporary table
        std::string tempTableName = "batyr_" + job->getId();

        auto versionPostgis = Db::PostGis::getVersion(*(transaction.get()));

        // create a temp table to write the data to
        transaction->createTempTable(layer->target_table_schema, layer->target_table_name, tempTableName);

        // fetch the column list from the target_table as the tempTable
        // does not have the constraints of the original table
        auto tableFields = transaction->getTableFields(layer->target_table_schema, layer->target_table_name);

        // check if the requirements of the primary key are satisfied
        std::vector<std::string> primaryKeyColumns;
        std::string geometryColumn;
        std::vector<std::string> insertColumns;
        std::vector<std::string> updateColumns;
        for(const auto &tableFieldPair : tableFields) {
            if (tableFieldPair.second.isPrimaryKey) {
                primaryKeyColumns.push_back(tableFieldPair.second.name);
            }
            else {
                updateColumns.push_back(tableFieldPair.second.name);
            }
            if (tableFieldPair.second.pgTypeName == "geometry") {
                if (!geometryColumn.empty()) {
                    throw WorkerError("Layer \"" + job->getLayerName() + "\" has multiple geometry columns. Currently only one is supported");
                }
                geometryColumn = tableFieldPair.second.name;
                insertColumns.push_back(tableFieldPair.second.name);
            }
            if (ogrFields.find(tableFieldPair.second.name) != ogrFields.end() ||
                ogrLayer->GetFIDColumn() == tableFieldPair.second.name) {
                insertColumns.push_back(tableFieldPair.second.name);
            }
        }
        // allow overriding the primarykey from the configfile if there are alternatives configured there
        if (!layer->primary_key_columns.empty()) {
            for(const auto primary_key_column : layer->primary_key_columns) {
                if (tableFields.find(primary_key_column) == tableFields.end()) {
                    throw WorkerError("The configured primary key column \"" + primary_key_column + "\" does not exist in the table"
                            " of layer \"" + job->getLayerName() + "\"");
                }
            }
            primaryKeyColumns = layer->primary_key_columns;
        }
        if (primaryKeyColumns.empty()) {
            throw WorkerError("Got no primarykey for layer \"" + job->getLayerName() + "\"");
        }
        std::vector<std::string> missingPrimaryKeysSource;
        for (const auto &primaryKeyCol : primaryKeyColumns) {
            if (ogrFields.find(primaryKeyCol) == ogrFields.end() &&
                ogrLayer->GetFIDColumn() != primaryKeyCol) {
                missingPrimaryKeysSource.push_back(primaryKeyCol);
            }
        }
        if (!missingPrimaryKeysSource.empty()) {
            throw WorkerError("The source for layer \"" + job->getLayerName() + "\" is missing the following fields required "+
                    "by the primary key: " + StringUtils::join(missingPrimaryKeysSource, ", "));
        }

        // fetch the srid used for the column in postgis
        int pgSrid = POSTGIS_NO_SRID_FOUND;
        int pgUndefinedSrid = Db::PostGis::getUndefinedSRIDValue(versionPostgis);
        if (!geometryColumn.empty()) {
            pgSrid = Db::PostGis::getGeometryColumnSRID(*(transaction.get()),
                        layer->target_table_schema,
                        layer->target_table_name, geometryColumn);
            poco_debug(logger, "table " + layer->target_table_schema + "." + layer->target_table_name +
                        " column " + geometryColumn + " uses SRID " + std::to_string(pgSrid));
        }

        // prepare an insert query into the temporary table
        std::vector<std::string> insertQueryValues;
        unsigned int idxColumn = 1;
        for (const std::string &insertColumn : insertColumns) {
            auto tableField = &tableFields[insertColumn];
            std::stringstream colStream;

            if (tableField->pgTypeName != "geometry") {
                auto ogrField = &ogrFields[insertColumn];

                colStream   << "$" << idxColumn
                            << "::" << getPostgresType(ogrField->type)
                            << "::" << tableField->pgTypeName;
            }
            else {
                std::stringstream logStream;
                logStream << "job " << job->getId() << " geometry_columns for " << insertColumn;

                if (pgSrid == POSTGIS_NO_SRID_FOUND) {
                    logStream   << " contains no SRID information."
                                << " Reprojection is impossible -> using the SRID of the geometries as they are read from the source.";

                    colStream   <<  "$" << idxColumn << "::text::" << tableField->pgTypeName;
                }
                else {
                    // all srids smaller than 1 are treated as undefined.
                    // see http://lists.osgeo.org/pipermail/postgis-devel/2011-October/015413.html
                    if (pgSrid <= 0) {
                        logStream   << " returns SRID=" << pgSrid << " (undefined)."
                                    << " Reprojection is impossible -> assigning the SRID=" << pgUndefinedSrid << " (native undefined) to the new geometries";

                        colStream   << "st_setsrid($" << idxColumn << "::text::" << tableField->pgTypeName << ", "
                                    << pgUndefinedSrid << ")";
                    }
                    else {
                        logStream   << " returns SRID=" << pgSrid << "."
                                    << " Reprojecting geometries with a SRS, assigning SRID=" << pgSrid << " to incomming geometries without SRS.";

                        // in case the geometries do not have a SRS, assign the one of the table to them
                        colStream   << "(select "
                                    <<      "case when st_srid(foo.g) = " << pgUndefinedSrid << " then "
                                    <<          " st_setsrid(foo.g, " << pgSrid << ") "
                                    <<      "else "
                                    <<          " st_transform(foo.g, " << pgSrid << ") "
                                    <<      "end"
                                    << " from ( select $" << idxColumn << "::text::" << tableField->pgTypeName << " as g "
                                    << " ) foo)";
                    }
                }
                poco_information(logger, logStream.str().c_str());
            }

            insertQueryValues.push_back(colStream.str());
            idxColumn++;
        }
        std::stringstream insertQueryStream;
        insertQueryStream   << "insert into " << transaction->quoteIdent(tempTableName) << " ("
                            << StringUtils::join(transaction->quoteIdent(insertColumns), ", ")
                            << ") "
                            << "select "
                            << StringUtils::join(insertQueryValues, ", ");
        poco_debug(logger, insertQueryStream.str().c_str());
        std::string insertStmtName = "batyr_insert" + job->getId();
        auto resInsertStmt = transaction->prepare(insertStmtName, insertQueryStream.str(), insertColumns.size(), NULL);

        OGRFeature * ogrFeatureP = 0;
        // ensure that features get free'd by wraping them in a smart pointer
        std::unique_ptr<OGRFeature, decltype((OGRFeature::DestroyFeature))> ogrFeature(
                NULL ,  OGRFeature::DestroyFeature);

        while( (ogrFeatureP = ogrLayer->GetNextFeature()) != nullptr) {
            ogrFeature.reset(ogrFeatureP);

            std::vector<QueryValue> pgValues;

            for (const std::string &insertColumn : insertColumns) {
                auto tableField = &tableFields[insertColumn];

                if (tableField->pgTypeName == "geometry") {

                    QueryValue pgVal;
                    auto ogrGeometry = ogrFeature->GetGeometryRef();
                    if (ogrGeometry != nullptr) {
                        // TODO: Maybe use the implementation from OGRPGLayer::GeometryToHex
                        GByte * buffer;
                        int bufferSize = ogrGeometry->WkbSize();

                        buffer = (GByte *) CPLMalloc(bufferSize);
                        if (buffer == nullptr) {
                            throw WorkerError("Unable to allocate memory to export geometry");
                        }
                        if (ogrGeometry->exportToWkb(wkbNDR, buffer) != OGRERR_NONE) {
                            OGRFree(buffer);
                            throw WorkerError("Could not export the geometry from feature #" + std::to_string(numPulled));
                        }
                        char * hexBuffer = CPLBinaryToHex(bufferSize, buffer);
                        if (hexBuffer == nullptr) {
                            OGRFree(buffer);
                            throw WorkerError("Unable to allocate memory to convert geometry to hex");
                        }
                        OGRFree(buffer);
                        pgVal.setIsNull(bufferSize == 0);
                        if (!pgVal.isNull()) {
                            pgVal.set(std::string(hexBuffer));
                        }
                        CPLFree(hexBuffer);
                    }
                    else {
                        pgVal.setIsNull(true);
                    }
                    pgValues.push_back(std::move(pgVal));
                }
                else {
                    if (ogrLayer->GetFIDColumn() == insertColumn) {
                        // handle special case where insertColumn is the fid
                        // with index = -1
                        QueryValue pV = convertFidToString(ogrFeature.get());
                        pgValues.push_back( std::move(pV) );
                    } else {
                        auto ogrField = &ogrFields[insertColumn];
                        QueryValue pV = convertToString(ogrFeature.get(), ogrField->index, ogrField->type, tableField->pgTypeName);
                        pgValues.push_back( std::move(pV) );
                    }
                }
            }

            if (layer->ignore_failures) {
                try {
                    transaction->exec("savepoint insertfeature;");
                    transaction->execPrepared(insertStmtName, pgValues);
                    transaction->exec("release savepoint insertfeature;");
                }
                catch (Batyr::Db::DbError &e) {
                    if (!e.isDataException()) {
                        throw;
                    }
                    numIgnored++;
                    transaction->exec("rollback to savepoint insertfeature;");

                    std::stringstream ignoreMsgStream;
                    ignoreMsgStream << "Ignoring feature: " << e.what();
                    poco_warning(logger, ignoreMsgStream.str().c_str());
                }
            }
            else {
                transaction->execPrepared(insertStmtName, pgValues);
            }
            numPulled++;
        }
        job->setStatistics(numPulled, numCreated, numUpdated, numDeleted, numIgnored);

        // Update data using bulk mode if according option was set.
        if (layer->bulk_mode) {

            //
            // Delete or truncate all records from target table.
            //
            if (layer->bulk_delete_method == BULK_TRUNCATE) {
                // Firstly count records in target table because PQcmdTuples() will not return a
                // number of truncated records.
                // Note: count function with primary key column should be faster than count(*) as
                // it is an index in Postgres.
                // Note: primaryKeyColumns[] was already checked for emptiness.
                std::stringstream countStmt;
                countStmt << "select count(" << primaryKeyColumns[0] << ") from " << transaction->quoteAndJoinIdent(layer->target_table_schema, layer->target_table_name);
                auto countRes = transaction->exec(countStmt.str());
                numDeleted = std::atoi(PQgetvalue(countRes.get(),0,0));
                countRes.reset(NULL);
                // Than truncate table.
                std::stringstream truncateStmt;
                truncateStmt   << "truncate " << transaction->quoteAndJoinIdent(layer->target_table_schema, layer->target_table_name);
                auto truncateRes = transaction->exec(truncateStmt.str());
                truncateRes.reset(NULL);
            } else {
                std::stringstream deleteStmt;
                deleteStmt   << "delete from " << transaction->quoteAndJoinIdent(layer->target_table_schema, layer->target_table_name);
                auto deleteRes = transaction->exec(deleteStmt.str());
                numDeleted = std::atoi(PQcmdTuples(deleteRes.get()));
                deleteRes.reset(NULL);
            }

            //
            // Insert all records from the temp table to the taget table.
            //
            std::stringstream insertStmt;
            // Note: temp table already has the same columns structure as the target table but we need
            // an order of columns to correctly insert data.
            // Note: insertColumns contains all columns even primary key and geometry columns.
            insertStmt   << "insert into " << transaction->quoteAndJoinIdent(layer->target_table_schema, layer->target_table_name)
                         << " ( " << StringUtils::join(transaction->quoteIdent(insertColumns), ", ") << ") "
                         << " select " << StringUtils::join(transaction->quoteIdent(insertColumns), ", ") << " "
                         << " from " << transaction->quoteIdent(tempTableName);
            auto insertRes = transaction->exec(insertStmt.str());
            numCreated = std::atoi(PQcmdTuples(insertRes.get()));
            insertRes.reset(NULL);

        // Update data in a default way.
        } else {

            //
            // update the existing/target table
            //
            std::stringstream updateStmt;
            updateStmt          << "update " << transaction->quoteAndJoinIdent(layer->target_table_schema, layer->target_table_name) << " "
                                << " set ";
            for (size_t i=0; i<updateColumns.size(); i++) {
                if (i != 0) {
                    updateStmt << ", ";
                }
                updateStmt  << transaction->quoteIdent(updateColumns[i]) << " = "
                            << transaction->quoteAndJoinIdent(tempTableName, updateColumns[i]) << " ";
            }
            updateStmt          << " from " << transaction->quoteIdent(tempTableName)
                                << " where (";
            for (size_t i=0; i<primaryKeyColumns.size(); i++) {
                if (i != 0) {
                    updateStmt << " and ";
                }
                updateStmt  << transaction->quoteAndJoinIdent(layer->target_table_name, primaryKeyColumns[i])
                            << " is not distinct from "
                            << transaction->quoteAndJoinIdent(tempTableName, primaryKeyColumns[i]);
            }
            updateStmt          << ") and (";

            // add more WHERE conditions to only update rows which are actually different.
            // There is no purpose in performing updates when none of the colums changed. This will only
            // fire eventually exisiting triggers which would make the operation more expensive
            // ... and that should be avoided.
            for (size_t i=0; i<updateColumns.size(); i++) {
                if (i != 0) {
                    updateStmt << " or ";
                }

                auto tableField = &tableFields[updateColumns[i]];

                if (tableField->pgTypeName == "geometry") {
                    std::string quotedTargetGeom =  transaction->quoteAndJoinIdent(layer->target_table_name, updateColumns[i]);
                    std::string quotedTempGeom = transaction->quoteAndJoinIdent(tempTableName, updateColumns[i]);

                    // update geometries always when the srid differs or when they are collections.
                    // MEMO: geometries with different SRIDs can not be compared with the "=" operator
                    // MEMO: collections can not be compared with st_equals
                    updateStmt  << "("
                                <<      "case when "
                                <<          "(st_srid(" << quotedTargetGeom << ") != st_srid(" << quotedTempGeom << ")) ";

                    if (std::get<0>(versionPostgis) >= 2) {
                            // st_iscollection is only supported starting with postgis 2.0
                            updateStmt  << " or st_iscollection(" << quotedTargetGeom << ") "
                                        << " or st_iscollection(" << quotedTempGeom << ") ";
                    }
                    else {
                            updateStmt  << " or ("
                                        <<   "st_geometrytype(" << quotedTargetGeom << ") = 'ST_GeometryCollection'"
                                        <<   " or st_geometrytype(" << quotedTargetGeom << ") like 'ST_Multi%'"
                                        << ") "
                                        << " or ("
                                        <<   "st_geometrytype(" << quotedTempGeom << ") = 'ST_GeometryCollection'"
                                        <<   " or st_geometrytype(" << quotedTempGeom << ") like 'ST_Multi%'"
                                        << ") ";
                    }

                    updateStmt  <<  "then "
                                // compare using the binary representation as ST_Equals can not be used in this case
                                <<      quotedTargetGeom << "::bytea " << " is distinct from " << quotedTempGeom << "::bytea "
                                <<  " else "
                                // compare using st_equals
                                <<      "not st_equals("  << quotedTargetGeom << ", " << quotedTempGeom << ")"
                                <<  " end "
                                << ")";
                }
                else {
                    updateStmt  << "(" << transaction->quoteAndJoinIdent(layer->target_table_name, updateColumns[i])
                                << " is distinct from "
                                << transaction->quoteAndJoinIdent(tempTableName, updateColumns[i]) << ")";
                }
            }
            updateStmt          << ")";
            auto updateRes = transaction->exec(updateStmt.str());
            numUpdated = std::atoi(PQcmdTuples(updateRes.get()));
            updateRes.reset(NULL); // immediately dispose the result

            //
            // insert missing rows in the exisiting/target table
            //
            std::stringstream insertMissingStmt;
            insertMissingStmt   << "insert into " << transaction->quoteAndJoinIdent(layer->target_table_schema, layer->target_table_name)
                                << " ( " << StringUtils::join(transaction->quoteIdent(insertColumns), ", ") << ") "
                                << " select " << StringUtils::join(transaction->quoteIdent(insertColumns), ", ") << " "
                                << " from " << transaction->quoteIdent(tempTableName)
                                << " where (" << StringUtils::join(transaction->quoteIdent(primaryKeyColumns), ", ") << ") not in ("
                                << " select " << StringUtils::join(transaction->quoteIdent(primaryKeyColumns), ",") << " "
                                << "       from " << transaction->quoteAndJoinIdent(layer->target_table_schema, layer->target_table_name)
                                << ")";
            auto insertMissingRes = transaction->exec(insertMissingStmt.str());
            numCreated = std::atoi(PQcmdTuples(insertMissingRes.get()));
            insertMissingRes.reset(NULL); // immediately dispose the result

            //
            // delete deprecated rows from the exisiting/target table
            //
            if (allow_feature_deletion) {
                std::stringstream deleteRemovedStmt;
                auto quotedPrimaryKeyColumnsStr = StringUtils::join(transaction->quoteIdent(primaryKeyColumns), ", ");
                deleteRemovedStmt   << "delete from " << transaction->quoteAndJoinIdent(layer->target_table_schema, layer->target_table_name)
                                    << " where (" << quotedPrimaryKeyColumnsStr << ") not in ("
                                    << " select " << quotedPrimaryKeyColumnsStr << " "
                                    << "       from " << transaction->quoteIdent(tempTableName)
                                    << ")";
                auto deleteRemovedRes = transaction->exec(deleteRemovedStmt.str());
                numDeleted = std::atoi(PQcmdTuples(deleteRemovedRes.get()));
                deleteRemovedRes.reset(NULL); // immediately dispose the result
            }
        }

        job->setStatus(Job::Status::FINISHED);
        job->setStatistics(numPulled, numCreated, numUpdated, numDeleted, numIgnored);

        std::stringstream finalLogMsgStream;
        finalLogMsgStream   << "job " << job->getId() << " finished. Stats: "
                            << "pulled=" << numPulled << ", "
                            << "created=" << numCreated << ", "
                            << "updated=" << numUpdated << ", "
                            << "deleted=" << numDeleted;
        if (!allow_feature_deletion) {
            finalLogMsgStream << " (feature deletion is disabled in configuration)";
        }
        poco_information(logger, finalLogMsgStream.str().c_str());
    }
    else {
        std::string msg("Could not start a database transaction");
        poco_error(logger, msg.c_str());
        job->setStatus(Job::Status::FAILED);
        job->setMessage(msg);
    }

}


void
Worker::removeByAttributes(Job::Ptr job)
{
    {
        std::stringstream initialLogMsgStream;
        initialLogMsgStream     << "job " << job->getId()
                                << ": remove-by-attributes on layer \"" << job->getLayerName() << "\"";
        poco_information(logger, initialLogMsgStream.str().c_str());
    }

    auto layer = configuration->getLayer(job->getLayerName());

    if (!layer->allow_feature_deletion) {
        std::string msg = "Layer \"" + job->getLayerName() + "\" does not allow deletion of features.";
        poco_warning(logger, msg.c_str());
        job->setStatus(Job::Status::FAILED);
        job->setMessage(msg);
        return;
    }



    // perform the work in an transaction
    if (auto transaction = db.getTransaction()) {

        // set the postgresql date style
        transaction->exec("set DateStyle to SQL, YMD");

        // fetch the column list from the target_table as the tempTable
        // does not have the constraints of the original table
        auto tableFields = transaction->getTableFields(layer->target_table_schema, layer->target_table_name);

        std::stringstream deleteStmt;
        deleteStmt  << "delete from " << transaction->quoteAndJoinIdent(layer->target_table_schema, layer->target_table_name)
                    << " where ";

        int numDeleted = 0;
        std::vector<Job::AttributeValue> attrValues;
        for(const auto & attributeSet: job->getAttributeSets()) {
            if (attributeSet.size() > 0) {
                int i = 0;
                if (!attrValues.empty()) {
                    deleteStmt << " or ";
                }
                deleteStmt << " ( ";
                for(const auto & attributePair: attributeSet) {
                    auto tableFieldIt = tableFields.find(attributePair.first);
                    if (tableFieldIt == tableFields.end()) {
                        throw WorkerError("Layer \"" + job->getLayerName() + "\" has no field \""
                                        + attributePair.first + "\"");
                    }
                    if (i > 0) {
                        deleteStmt << " and ";
                    }
                    attrValues.push_back(attributePair.second);
                    deleteStmt  << transaction->quoteAndJoinIdent(layer->target_table_name, attributePair.first)
                                << " is not distinct from $" << attrValues.size() << "::" << tableFieldIt->second.pgTypeName;
                    ++i;
                }
                deleteStmt << " ) ";
            }
        }

        if (!attrValues.empty()) {
            poco_debug(logger, deleteStmt.str().c_str());

            auto deleteRes = transaction->execParams(deleteStmt.str(), attrValues);
            numDeleted = std::atoi(PQcmdTuples(deleteRes.get()));
        }
        else {
            std::stringstream logMsgStream;
            logMsgStream   << "job " << job->getId() << ": no attributes to remove features given. skipping";
            poco_information(logger, logMsgStream.str().c_str());
        }

        job->setStatus(Job::Status::FINISHED);
        job->setStatistics(0, 0, 0, numDeleted);

        std::stringstream finalLogMsgStream;
        finalLogMsgStream   << "job " << job->getId() << " finished. Stats: "
                            << "deleted=" << numDeleted;
        poco_information(logger, finalLogMsgStream.str().c_str());

    }
    else {
        std::string msg("Could not start a database transaction");
        poco_error(logger, msg.c_str());
        job->setStatus(Job::Status::FAILED);
        job->setMessage(msg);
    }
}


void
Worker::run()
{
    while (true) {
        Job::Ptr job;
        try {
            jobs->popNoWait(job);
            if (!job) {
                // shut down the db connection if this behaviour is configured and
                if (!configuration->usePersistentDbConnections()) {
                    poco_debug(logger, "No jobs queued - closing db connection");
                    db.close();
                }

                // wait for a new job to arrive
                // getting still no job means the queue recieved a quit command, so the worker
                // can be shut down
                jobs->popWait(job);
                if (!job) {
                    break;
                }
            }
            poco_debug(logger, "Got job from queue");
            job->setStatus(Job::Status::IN_PROCESS);

            // check if we got a working database connection
            // or block until we got one
            size_t reconnectAttempts = 0;
            while(!db.reconnect(true)) {
                if (reconnectAttempts == 0) {
                    // set job message to inform clients we are waiting here
                    job->setMessage("Waiting to aquire a database connection");
                }
                reconnectAttempts++;
                std::this_thread::sleep_for( std::chrono::milliseconds( SERVER_DB_RECONNECT_WAIT ) );
            }
            job->setMessage("");

            switch(job->getType()) {
                case Job::Type::PULL:
                    pull(job);
                    break;
                case Job::Type::REMOVE_BY_ATTRIBUTES:
                    removeByAttributes(job);
                    break;
            }
        }
        catch (Batyr::Db::DbError &e) {
            poco_error(logger, e.what());
            if (e.hasContext()) {
                poco_error(logger, "postgresql error context: " + e.getContext());
            }
            job->setStatus(Job::Status::FAILED);
            job->setMessage(e.what());
        }
        catch (WorkerError &e) {
            poco_error(logger, e.what());
            job->setStatus(Job::Status::FAILED);
            job->setMessage(e.what());
        }
        catch (std::runtime_error &e) {
            poco_error(logger, e.what());
            job->setStatus(Job::Status::FAILED);
            job->setMessage(e.what());

            // do not know how this exception was caused as it
            // was not handled by one of the earlier catch blocks
            throw;
        }
    }
    poco_debug(logger, "leaving run method");
}


std::string
Worker::getPostgresType(OGRFieldType fieldType)
{
    std::string pgFieldType;

    switch (fieldType) {
        case OFTString:
            pgFieldType = "text";
            break;
        case OFTInteger:
            pgFieldType = "integer";
            break;
        case OFTReal:
            pgFieldType = "double precision";
            break;
        case OFTDate:
            pgFieldType = "date";
            break;
        case OFTTime:
            pgFieldType = "time";
            break;
        case OFTDateTime:
            pgFieldType = "timestamp";
            break;
        case OFTIntegerList:
            pgFieldType = "integer[]";
            break;
        case OFTRealList:
            pgFieldType = "double precision[]";
            break;
        case OFTStringList:
            pgFieldType = "text[]";
            break;
        case OFTBinary:
            pgFieldType = "bytea";
            break;
#if GDAL_VERSION_MAJOR > 1
        case OFTInteger64:
            pgFieldType = "integer";
            break;
        case OFTInteger64List:
            pgFieldType = "integer[]";
            break;
#endif
        default:
            throw WorkerError("No supported PostgreSQL type for OGR field type: " + std::to_string(static_cast<int>(fieldType)));
    }
    return pgFieldType;
}


QueryValue
Worker::convertToString(OGRFeature * ogrFeature, const int fieldIdx, OGRFieldType fieldType, const std::string pgTypeName)
{
    QueryValue result;

    switch (fieldType) {
        case OFTString:
            result.setIsNull(ogrFeature->IsFieldSet(fieldIdx) == 0);
            if (!result.isNull()) {
                const char * fieldValue = ogrFeature->GetFieldAsString(fieldIdx);
                result.set(fieldValue);
            }
            break;
        case OFTInteger:
#if GDAL_VERSION_MAJOR > 1
        case OFTInteger64:
#endif
            result.setIsNull(ogrFeature->IsFieldSet(fieldIdx) == 0);
            if (!result.isNull()) {
                result.set(std::to_string(ogrFeature->GetFieldAsInteger(fieldIdx)));
            }
            break;
        case OFTReal:
            result.setIsNull(ogrFeature->IsFieldSet(fieldIdx) == 0);
            if (!result.isNull()) {
                result.set(std::to_string(ogrFeature->GetFieldAsDouble(fieldIdx)));
            }
            break;
        case OFTDate:
        case OFTTime:
        case OFTDateTime:
            {
                int dtYear = 0;
                int dtMonth = 0;
                int dtDay = 0;
                int dtHour = 0;
                int dtMinute = 0;
                int dtSecond = 0;
                int dtTZFlag = 0;

                if (ogrFeature->GetFieldAsDateTime(fieldIdx, &dtYear, &dtMonth, &dtDay,
                            &dtHour, &dtMinute, &dtSecond, &dtTZFlag)) {
                    char buf[128] = {0,};

                    if (fieldType == OFTDate) {
                        std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", dtYear, dtMonth, dtDay);
                    }
                    else if (fieldType == OFTTime) {
                        std::snprintf(buf, sizeof(buf), "%02d:%02d:%09d.0", dtHour, dtMinute, dtSecond);
                    }
                    else if (fieldType == OFTDateTime) {
                        std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%09d.0",
                                 dtYear, dtMonth, dtDay, dtHour, dtMinute, dtSecond);
                    }
                    else {
                        throw WorkerError("None of the implemented date/time/datetime types matched. This point should not be reacheable");
                    }
                    result.setIsNull(false);
                    result.set(std::string(buf));
                }
                else {
                    result.setIsNull(true);
                    if (ogrFeature->IsFieldSet(fieldIdx) == 1) {
                        poco_debug(logger, "field at index " + std::to_string(fieldIdx) + ""
                                           " null, but also set.");
                    }
                }
                break;
            }
        case OFTIntegerList:
#if GDAL_VERSION_MAJOR > 1
        case OFTInteger64List:
#endif
        case OFTRealList:
        case OFTStringList:
            {
                std::stringstream valueStream;
                valueStream << "{";
                if (fieldType == OFTIntegerList) {
                    int listLen = 0;
                    const int *listValues = ogrFeature->GetFieldAsIntegerList(fieldIdx, &listLen);
                    for (int i=0; i<listLen; i++) {
                        if (i>0) {
                            valueStream << ",";
                        }
                        valueStream << listValues[i];
                    }
                }
                else if (fieldType == OFTRealList) {
                    int listLen = 0;
                    const double *listValues = ogrFeature->GetFieldAsDoubleList(fieldIdx, &listLen);
                    for (int i=0; i<listLen; i++) {
                        if (i>0) {
                            valueStream << ",";
                        }
                        valueStream << listValues[i];
                    }
                }
                else if (fieldType == OFTStringList) {
                    char **listValues = ogrFeature->GetFieldAsStringList(fieldIdx);
                    int listLen = CSLCount(listValues);
                    for (int i=0; i<listLen; i++) {
                        if (i>0) {
                            valueStream << ",";
                        }
                        std::string currentValue(listValues[i]);

                        // escape backslash and quotes in the string with a backslash to 
                        // build a valid array
                        // TODO: Escape using postgresqls quite_literal? Will issue lots of queries
                        StringUtils::replaceAll(currentValue, "\\", "\\\\");
                        StringUtils::replaceAll(currentValue, "\"", "\\\"");

                        valueStream << "\"" << currentValue << "\"";
                    }
                }
                else {
                    throw WorkerError("None of the implemented list types matched. This point should not be reacheable");
                }

                valueStream << "}";

                result.setIsNull(false);
                result.set(valueStream.str());
            }
        case OFTBinary:
        default:
            throw WorkerError("Unsupported OGR field type to convert to string: " + std::to_string(static_cast<int>(fieldType)));
    }

    // sanitize invalid values - empty time types are generaly invalid and most certainly result from
    // string fields returned as empty strings
    if (!result.isNull() && result.get().empty() && (
                (pgTypeName == "timestamp") ||
                (pgTypeName == "timestampz") ||
                (pgTypeName == "time") ||
                (pgTypeName == "date")
            )) {
        result.setIsNull(true);
    }

    return std::move(result);
}

QueryValue
Worker::convertFidToString(OGRFeature * ogrFeature)
{
    QueryValue result;

    // fid is always an integer
    // http://www.gdal.org/classOGRFeature.html#a45da957be1eb8aa824e3ee9dbfb9604c

    result.setIsNull(false);
    result.set(std::to_string(ogrFeature->GetFID()));

    return std::move(result);
}
