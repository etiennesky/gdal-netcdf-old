/*************************************************************************
 * File :    postgisrasterdataset.cpp
 * Project:  PostGIS Raster driver
 * Purpose:  GDAL Dataset implementation for PostGIS Raster driver 
 * Author:   Jorge Arevalo, jorge.arevalo@deimos-space.com
 * 
 * Last changes:
 * $Id:$
 *
 ************************************************************************
 * Copyright (c) 2010, Jorge Arevalo, jorge.arevalo@deimos-space.com
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the 
 * "Software"), to deal in the Software without restriction, including 
 * without limitation the rights to use, copy, modify, merge, publish, 
 * distribute, sublicense, and/or sell copies of the Software, and to 
 * permit persons to whom the Software is furnished to do so, subject to 
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF 
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. 
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY 
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, 
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE 
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 ************************************************************************/

#include "postgisraster.h"
#include <stdlib.h>
#include "ogr_api.h"
#include "ogr_geometry.h"
#include "gdal.h"
#include "cpl_conv.h"
#include "cpl_string.h"
#include "gdal_priv.h"
#include <math.h>
#include "cpl_error.h"
#include "ogr_core.h"

#ifdef _WIN32
#define rint(x) floor((x) + 0.5)
#endif

CPL_C_START
void GDALRegister_PostGISRaster(void);

CPL_C_END



/************************
 * \brief Constructor
 ************************/
PostGISRasterDataset::PostGISRasterDataset() {
    papszSubdatasets = NULL;
    nSrid = -1;
    poConn = NULL;
    bRegularBlocking = false;
    bRegisteredInRasterColumns = false;
    pszSchema = NULL;
    pszTable = NULL;
    pszColumn = NULL;
    pszWhere = NULL;
    pszProjection = NULL;
    nMode = NO_MODE;
    poDriver = NULL;
    nBlockXSize = 0;
    nBlockYSize = 0;
    adfGeoTransform[0] = 0.0;     /* X Origin (top left corner) */
    adfGeoTransform[1] = 1.0;     /* X Pixel size */
    adfGeoTransform[2] = 0.0;
    adfGeoTransform[3] = 0.0;     /* Y Origin (top left corner) */
    adfGeoTransform[4] = 0.0;
    adfGeoTransform[5] = 1.0;     /* Y Pixel Size */
}

/************************
 * \brief Constructor
 ************************/
PostGISRasterDataset::~PostGISRasterDataset() {

    if (pszSchema)
        CPLFree(pszSchema);
    if (pszTable)
        CPLFree(pszTable);
    if (pszColumn)
        CPLFree(pszColumn);
    if (pszWhere)
        CPLFree(pszWhere);
    if (pszProjection)
        CPLFree(pszProjection);

    if (papszSubdatasets)
        CSLDestroy(papszSubdatasets);

    PQfinish(poConn);

    //delete poDriver;
}

/**************************************************************
 * \brief Replace the single quotes by " in the input string
 *
 * Needed before tokenize function
 *************************************************************/
static
char * ReplaceSingleQuotes(const char * pszInput, int nLength) {
    int i;
    char* pszOutput = NULL;

    if (nLength == -1)
        nLength = strlen(pszInput);

    pszOutput = (char*) CPLCalloc(nLength + 1, sizeof (char));

    for (i = 0; i < nLength; i++) {
        if (pszInput[i] == '\'')
            pszOutput[i] = '"';
        else
            pszOutput[i] = pszInput[i];

    }

    return pszOutput;
}


/**************************************************************
 * \brief Replace the quotes by single quotes in the input string
 *
 * Needed in the 'where' part of the input string
 *************************************************************/
static
char * ReplaceQuotes(const char * pszInput, int nLength) {
    int i;
    char * pszOutput = NULL;

    if (nLength == -1)
        nLength = strlen(pszInput);

    pszOutput = (char*) CPLCalloc(nLength + 1, sizeof (char));

    for(i = 0; i < nLength; i++) {
        if (pszInput[i] == '"')
            pszOutput[i] = '\'';
        else
            pszOutput[i] = pszInput[i];
    }

    return pszOutput;
}

/*****************************************************************************
 * \brief Split connection string into user, password, host, database...
 *
 * The parameters separated by spaces are return as a list of strings. The
 * function accepts all the PostgreSQL recognized parameter key words.
 *
 * The returned list must be freed with CSLDestroy when no longer needed
 *
 *****************************************************************************/
static
char** ParseConnectionString(const char * pszConnectionString) {
    char * pszEscapedConnectionString = NULL;

    /* Escape string following SQL scheme */
    pszEscapedConnectionString = ReplaceSingleQuotes(pszConnectionString, -1);

    /* Avoid PG: part */
    char* pszStartPos = (char*) strstr(pszEscapedConnectionString, ":") + 1;

    /* Tokenize */
    char** papszParams = CSLTokenizeString2(pszStartPos, " ",
            CSLT_HONOURSTRINGS);

    /* Free */
    CPLFree(pszEscapedConnectionString);

    return papszParams;

}

/**************************************************************************
 * \brief Look for raster tables in database and store them as subdatasets
 *
 * If no table is provided in connection string, the driver looks for the
 * existent raster tables in the schema given as argument. This argument,
 * however, is optional. If a NULL value is provided, the driver looks for
 * all raster tables in all schemas of the user-provided database.
 *
 * NOTE: Permissions are managed by libpq. The driver only returns an error
 * if an error is returned when trying to access to tables not allowed to
 * the current user.
 **************************************************************************/
GBool PostGISRasterDataset::BrowseDatabase(const char* pszCurrentSchema,
        char* pszValidConnectionString) {
    /* Be careful! These 3 vars override the class ones! */
    char* pszSchema = NULL;
    char* pszTable = NULL;
    char* pszColumn = NULL;
    int i = 0;
    int nTuples = 0;
    PGresult * poResult = NULL;
    CPLString osCommand;


    /*************************************************************
     * Fetch all the raster tables and store them as subdatasets
     *************************************************************/
    if (pszCurrentSchema == NULL) {
        osCommand.Printf("select pg_namespace.nspname as schema, pg_class.relname as \
					table, pg_attribute.attname as column from pg_class, \
					pg_namespace,pg_attribute, pg_type where \
					pg_class.relnamespace = pg_namespace.oid and pg_class.oid = \
					pg_attribute.attrelid and pg_attribute.atttypid = pg_type.oid \
					and pg_type.typname = 'raster'");

        poResult = PQexec(poConn, osCommand.c_str());
        if (
                poResult == NULL ||
                PQresultStatus(poResult) != PGRES_TUPLES_OK ||
                PQntuples(poResult) <= 0
                ) {
            CPLError(CE_Failure, CPLE_AppDefined,
                    "Error browsing database for PostGIS Raster tables: %s", PQerrorMessage(poConn));
            if (poResult != NULL)
                PQclear(poResult);

            return false;
        }


        nTuples = PQntuples(poResult);
        for (i = 0; i < nTuples; i++) {
            pszSchema = PQgetvalue(poResult, i, 0);
            pszTable = PQgetvalue(poResult, i, 1);
            pszColumn = PQgetvalue(poResult, i, 2);

            papszSubdatasets = CSLSetNameValue(papszSubdatasets,
                    CPLSPrintf("SUBDATASET_%d_NAME", (i + 1)),
                    CPLSPrintf("PG:%s schema=%s table=%s column=%s",
                    pszValidConnectionString, pszSchema, pszTable, pszColumn));

            papszSubdatasets = CSLSetNameValue(papszSubdatasets,
                    CPLSPrintf("SUBDATASET_%d_DESC", (i + 1)),
                    CPLSPrintf("PostGIS Raster table at %s.%s (%s)", pszSchema, pszTable, pszColumn));
        }

        PQclear(poResult);

    }
        /**********************************************************************
         * Fetch all the schema's raster tables and store them as subdatasets
         **********************************************************************/
    else {
        osCommand.Printf("select pg_class.relname as table, pg_attribute.attname \
			 as column from pg_class, pg_namespace,pg_attribute, pg_type where \
			 pg_class.relnamespace = pg_namespace.oid and pg_class.oid = \
			 pg_attribute.attrelid and pg_attribute.atttypid = pg_type.oid \
			 and pg_type.typname = 'raster' and pg_namespace.nspname = '%s'",
                pszCurrentSchema);

        poResult = PQexec(poConn, osCommand.c_str());
        if (
                poResult == NULL ||
                PQresultStatus(poResult) != PGRES_TUPLES_OK ||
                PQntuples(poResult) <= 0
                ) {
            CPLError(CE_Failure, CPLE_AppDefined,
                    "Error browsing database for PostGIS Raster tables: %s", PQerrorMessage(poConn));
            if (poResult != NULL)
                PQclear(poResult);

            return false;
        }


        nTuples = PQntuples(poResult);
        for (i = 0; i < nTuples; i++) {
            pszTable = PQgetvalue(poResult, i, 0);
            pszColumn = PQgetvalue(poResult, i, 1);

            papszSubdatasets = CSLSetNameValue(papszSubdatasets,
                    CPLSPrintf("SUBDATASET_%d_NAME", (i + 1)),
                    CPLSPrintf("PG:%s schema=%s table=%s column=%s",
                    pszValidConnectionString, pszCurrentSchema, pszTable, pszColumn));

            papszSubdatasets = CSLSetNameValue(papszSubdatasets,
                    CPLSPrintf("SUBDATASET_%d_DESC", (i + 1)),
                    CPLSPrintf("PostGIS Raster table at %s.%s (%s)", pszCurrentSchema,
                    pszTable, pszColumn));
        }

        PQclear(poResult);
    }

    return true;
}

/*************************************************************************
 * \brief Set the general raster properties.
 *
 * We must distinguish between tiled and untiled raster coverages. In
 * PostGIS Raster, there's no real difference between 'tile' and 'raster'.
 * There's only 'raster objects'. Each record of a raster table is a
 * raster object, and has its own georeference information, whether if
 * the record is a tile of a bigger raster coverage or is a complete
 * raster. So, <b>there's no a way of knowing if the rows of a raster
 * table are related or not</b>. It's user's responsibility. The only
 * thing driver can do is to suppose all the rows of a table are from
 * the same raster coverage if the user has queried for one table, without
 * specifying a where clause. 
 *
 * The user is responsible to ensure that the raster layer meets the minimum 
 * topological requirements for analysis. The ideal case is when all the raster 
 * tiles of a continuous layer are the same size, snap to the same grid and do 
 * not overlap.
 *
 * So, when we query for a raster table, we have 3 different cases:
 *	- If the result is only one row, we can gather the raster properties
 *	  from the returned object, regardless is a tile or a whole raster
 *	- If the result are several rows of a table, and the working mode is 
 *    ONE_RASTER_PER_TABLE, we assume all the rows are from the same raster 
 *    coverage. The rows are ordered by upper left y, upper left x, growing way, 
 *    and we can get raster size from the first and last elements.
 *  - If the result are several rows of a table, and the working mode is
 *    ONE_RASTER_PER_ROW, we assume each row is a different raster object,
 *    and is reported as a subdataset. If you want only one of the raster rows,
 *    you must specify a where clause to restrict the number of rows returned.
 **************************************************************************/
GBool PostGISRasterDataset::SetRasterProperties(const char* pszValidConnectionString) {
    PGresult* poResult = NULL;
    CPLString osCommand;
    CPLString osCommand2;
    PGresult* poResult2 = NULL;
    int i = 0;
    int nTuples = 0;
    int nRid = -1;
    GBool bRetValue = false;
    OGRSpatialReference * poSR = NULL;
    OGRGeometry* poGeom = NULL;
    OGRErr OgrErr = OGRERR_NONE;
    OGREnvelope* poE = NULL;
    char* pszExtent;
    char* pszProjectionRef;

    /* Execute the query to fetch raster data from db */
    if (pszWhere == NULL) {
        osCommand.Printf("select (foo.md).*, foo.rid from (select rid, st_metadata(%s) as md \
                        from %s.%s) as foo", pszColumn, pszSchema, pszTable);
    }
    else {
        osCommand.Printf("select (foo.md).*, foo.rid from (select rid, st_metadata(%s) as md \
                        from %s.%s where %s) as foo", pszColumn, pszSchema, pszTable, pszWhere);
    }

    poResult = PQexec(poConn, osCommand.c_str());
    if (
            poResult == NULL ||
            PQresultStatus(poResult) != PGRES_TUPLES_OK ||
            PQntuples(poResult) <= 0
            ) {
        CPLError(CE_Failure, CPLE_AppDefined,
                "Error browsing database for PostGIS Raster properties");
        if (poResult != NULL)
            PQclear(poResult);

        return false;
    }

    nTuples = PQntuples(poResult);


    /******************************************
     * Easier case. Only one raster to fetch
     ******************************************/
    if (nTuples == 1) {
        nSrid = atoi(PQgetvalue(poResult, 0, 8));
        nBands = atoi(PQgetvalue(poResult, 0, 9));
        adfGeoTransform[0] = atof(PQgetvalue(poResult, 0, 0)); //upperleft x
        adfGeoTransform[1] = atof(PQgetvalue(poResult, 0, 4)); //pixelsize x
        adfGeoTransform[2] = atof(PQgetvalue(poResult, 0, 6)); //skew x
        adfGeoTransform[3] = atof(PQgetvalue(poResult, 0, 1)); //upperleft y
        adfGeoTransform[4] = atof(PQgetvalue(poResult, 0, 7)); //skew y
        adfGeoTransform[5] = atof(PQgetvalue(poResult, 0, 5)); //pixelsize y

        nRasterXSize = atoi(PQgetvalue(poResult, 0, 2));
        nRasterYSize = atoi(PQgetvalue(poResult, 0, 3));

        /**
         * Not tiled dataset: The whole raster.
         * TODO: This is a bad idea, but there're some problems getting one
         * scanline per round (nBlockYSize = 1). Probably, the problem is in
         * IReadBlock method. Review it and fix it.
         */
        nBlockXSize = nRasterXSize;
        nBlockYSize = nRasterYSize;

        bRetValue = true;
    }
    else {
        switch (nMode) {
                /**
                 * Each row is a different raster. Create subdatasets, one per row
                 **/

            case ONE_RASTER_PER_ROW:

                for (i = 0; i < nTuples; i++) {
                    nRid = atoi(PQgetvalue(poResult, i, 10));
        
                    papszSubdatasets = CSLSetNameValue(papszSubdatasets,
                            CPLSPrintf("SUBDATASET_%d_NAME", (i + 1)),
                            CPLSPrintf("PG:%s schema=%s table=%s column=%s where='rid = %d'",
                            pszValidConnectionString, pszSchema, pszTable, pszColumn, nRid));

                    papszSubdatasets = CSLSetNameValue(papszSubdatasets,
                            CPLSPrintf("SUBDATASET_%d_DESC", (i + 1)),
                            CPLSPrintf("PostGIS Raster at %s.%s (%s), rid = %d", pszSchema,
                            pszTable, pszColumn, nRid));
                }

                /* Not a single raster fetched */
                nRasterXSize = 0;
                nRasterYSize = 0;

                bRetValue = true;
                break;

                /************************************************************
                 * All rows form a whole raster coverage. So, all rows must:
                 *  - have the same srid
                 *  - snap to the same grid
                 * TODO: Work even if this requisites are  not complained. For
                 * example, by:
                 *  - Resampling all the rows to the grid of the first one
                 *  - Providing a new grid alignment for all the rows, with a
                 *    maximum of 6 parameters: ulx, uly, pixelsizex, pixelsizey,
                 *    skewx, skewy or a minimum of 3 parameters: ulx, uly,
                 *    pixelsize (x and y pixel sizes are equal and both skew are
                 *    0).
                 ************************************************************/
            case ONE_RASTER_PER_TABLE:

                /**
                 * Get the rest of raster properties from this object
                 */
                nSrid = atoi(PQgetvalue(poResult, 0, 8));
                    
                nBands = atoi(PQgetvalue(poResult, 0, 9));
                adfGeoTransform[0] = atof(PQgetvalue(poResult, 0, 0)); //upperleft x
                adfGeoTransform[1] = atof(PQgetvalue(poResult, 0, 4)); //pixelsize x
                adfGeoTransform[2] = atof(PQgetvalue(poResult, 0, 6)); //skew x
                adfGeoTransform[3] = atof(PQgetvalue(poResult, 0, 1)); //upperleft y
                adfGeoTransform[4] = atof(PQgetvalue(poResult, 0, 7)); //skew y
                adfGeoTransform[5] = atof(PQgetvalue(poResult, 0, 5)); //pixelsize y

                /* WARNING: Only valid in case of regular blocking */
                nBlockXSize = atoi(PQgetvalue(poResult, 0, 2));
                nBlockYSize = atoi(PQgetvalue(poResult, 0, 3));
               
                /** 
                 * Get whole raster extent
                 **/
                if (pszWhere == NULL)
                    osCommand2.Printf("select st_astext(st_setsrid(st_extent(%s::geometry),%d)) from %s.%s",
                        pszColumn, nSrid, pszSchema, pszTable);
                else
                    osCommand2.Printf("select st_astext(st_setsrid(st_extent(%s::geometry),%d)) from %s.%s where %s",
                        pszColumn, nSrid, pszSchema, pszTable, pszWhere);
                

                poResult2 = PQexec(poConn, osCommand2.c_str());
                if (poResult2 == NULL ||
                        PQresultStatus(poResult2) != PGRES_TUPLES_OK ||
                        PQntuples(poResult2) <= 0) {
                    CPLError(CE_Failure, CPLE_AppDefined,
                            "Error calculating whole raster extent: %s",
                            PQerrorMessage(poConn));

                    if (poResult2 != NULL)
                        PQclear(poResult2);

                    if (poResult != NULL)
                        PQclear(poResult);

                    return false;
                }

                /* Construct an OGR object with the raster extent */
                pszExtent = PQgetvalue(poResult2, 0, 0);
                
                pszProjectionRef = (char*) GetProjectionRef();
                poSR = new OGRSpatialReference(pszProjectionRef);
                OgrErr = OGRGeometryFactory::createFromWkt(&pszExtent,
                        poSR, &poGeom);
                if (OgrErr != OGRERR_NONE) {
                    CPLError(CE_Failure, CPLE_AppDefined,
                            "Couldn't calculate raster extent");

                    if (poResult2)
                        PQclear(poResult2);

                    if (poResult != NULL)
                        PQclear(poResult);

                    return false;
                }               

                poE = new OGREnvelope();
                poGeom->getEnvelope(poE);

                nRasterXSize = (int)
                        fabs(rint((poE->MaxX - poE->MinX) / adfGeoTransform[1]));
                nRasterYSize = (int)
                        fabs(rint((poE->MaxY - poE->MinY) / adfGeoTransform[5]));

                /* Correction for upper left y coord*/

                /**
                 * TODO: Review this. Is a good algorithm?
                 * If the pixel size Y is negative, we can assume the raster's
                 * reference system uses cartesian coordinates, in which the
                 * origin is in lower-left corner, while the origin in an image
                 * is un upper-left corner. In this case, the upper left Y value
                 * will be MaxY from the envelope. Otherwise, it will be MinY.
                 **/
                adfGeoTransform[0] = poE->MinX;
                if (adfGeoTransform[5] >= 0.0)
                    adfGeoTransform[3] = poE->MinY;
                else
                    adfGeoTransform[3] = poE->MaxY;

                /* Free resources */
                OGRGeometryFactory::destroyGeometry(poGeom);
                delete poE;                                
                delete poSR;
                PQclear(poResult2);

                bRetValue = true;
                break;

                /* TODO: take into account more cases, if applies */
            default:
                CPLError(CE_Failure, CPLE_AppDefined,
                        "Error, incorrect working mode");

                bRetValue = false;
        }
    }

    PQclear(poResult);
    return bRetValue;
}

/*********************************************
 * \brief Set raster bands for this dataset
 *********************************************/
GBool PostGISRasterDataset::SetRasterBands() {
    GBool bSignedByte = false;
    int nBitDepth = 8;
    char* pszDataType = NULL;
    int iBand = 0;
    PGresult * poResult = NULL;
    CPLString osCommand;
    double dfNodata = 0.0;
    GDALDataType hDataType = GDT_Byte;
    int nTuples = 0;
    GBool bIsOffline = false;

    /* Create each PostGISRasterRasterBand using the band metadata */
    for (iBand = 0; iBand < nBands; iBand++) {
        /* Create query to fetch metadata from db */
        if (pszWhere == NULL) {
            osCommand.Printf("select (foo.md).* from (select" 
                    " distinct st_bandmetadata( %s, %d) as md from %s. %s) as foo",
                    pszColumn, iBand + 1, pszSchema, pszTable);
        }
        else {

            osCommand.Printf("select (foo.md).* from (select"
                    " distinct st_bandmetadata( %s, %d) as md from %s. %s where %s) as foo",
                    pszColumn, iBand + 1, pszSchema, pszTable, pszWhere);
        }

        poResult = PQexec(poConn, osCommand.c_str());
        nTuples = PQntuples(poResult);

        /* Error getting info from database */
        if (poResult == NULL || PQresultStatus(poResult) != PGRES_TUPLES_OK ||
                nTuples <= 0) {
            CPLError(CE_Failure, CPLE_AppDefined,
                    "Error getting band metadata: %s",
                    PQerrorMessage(poConn));
            if (poResult)
                PQclear(poResult);

            return false;
        }

        /**
         * If we have more than one record here is because there are several
         * rows, belonging to the same raster coverage, with different band
         * metadata values. An error must be raised. 
         *
         * TODO: Is there any way to fix this problem?
         *
         * TODO: Even when the difference between metadata values are only a
         * few decimal numbers (for example: 3.0000000 and 3.0000001) they're
         * different tuples. And in that case, they must be the same
         **/
        /*
        if (nTuples > 1) {
            CPLError(CE_Failure, CPLE_AppDefined, "Error, the \
                    ONE_RASTER_PER_TABLE mode can't be applied if the raster \
                    rows don't have the same metadata for band %d",
                    iBand + 1);
            PQclear(poResult);
            return false;
        }
        */


        /* Get metadata and create raster band objects */
        pszDataType = CPLStrdup(PQgetvalue(poResult, 0, 0));
        dfNodata = atof(PQgetvalue(poResult, 0, 2));
        bIsOffline = EQUALN(PQgetvalue(poResult, 0, 3), "t", sizeof (char));

        if (EQUALN(pszDataType, "1BB", 3 * sizeof (char))) {
            hDataType = GDT_Byte;
            nBitDepth = 1;
        }
        else if (EQUALN(pszDataType, "2BUI", 4 * sizeof (char))) {
            hDataType = GDT_Byte;
            nBitDepth = 2;
        }
        else if (EQUALN(pszDataType, "4BUI", 4 * sizeof (char))) {
            hDataType = GDT_Byte;
            nBitDepth = 4;
        }
        else if (EQUALN(pszDataType, "8BUI", 4 * sizeof (char))) {
            hDataType = GDT_Byte;
            nBitDepth = 8;
        }
        else if (EQUALN(pszDataType, "8BSI", 4 * sizeof (char))) {
            hDataType = GDT_Byte;
            /**
             * To indicate the unsigned byte values between 128 and 255
             * should be interpreted as being values between -128 and -1 for
             * applications that recognise the SIGNEDBYTE type.
             **/
            bSignedByte = true;
            nBitDepth = 8;
        } else if (EQUALN(pszDataType, "16BSI", 5 * sizeof (char))) {
            hDataType = GDT_Int16;
            nBitDepth = 16;
        }
        else if (EQUALN(pszDataType, "16BUI", 5 * sizeof (char))) {
            hDataType = GDT_UInt16;
            nBitDepth = 16;
        }
        else if (EQUALN(pszDataType, "32BSI", 5 * sizeof (char))) {
            hDataType = GDT_Int32;
            nBitDepth = 32;
        }
        else if (EQUALN(pszDataType, "32BUI", 5 * sizeof (char))) {
            hDataType = GDT_UInt32;
            nBitDepth = 32;
        }
        else if (EQUALN(pszDataType, "32BF", 4 * sizeof (char))) {
            hDataType = GDT_Float32;
            nBitDepth = 32;
        }
        else if (EQUALN(pszDataType, "64BF", 4 * sizeof (char))) {
            hDataType = GDT_Float64;
            nBitDepth = 64;
        }
        else {
            hDataType = GDT_Byte;
            nBitDepth = 8;
        }

        /* Create raster band object */
        SetBand(iBand + 1, new PostGISRasterRasterBand(this, iBand + 1, hDataType,
                dfNodata, bSignedByte, nBitDepth, 0, bIsOffline));

        CPLFree(pszDataType);
        PQclear(poResult);
    }

    return true;
}

/******************************************************************************
 * \brief Open a connection with PostgreSQL. The connection string will have 
 * the PostgreSQL accepted format, plus the next key=value pairs:
 *	schema = <schema_name>
 *	table = <table_name>
 *	column = <column_name>
 *	where = <SQL where>
 *  mode = <working mode> (1 or 2)
 *
 * These pairs are used for selecting the right raster table.
 *****************************************************************************/
GDALDataset* PostGISRasterDataset::Open(GDALOpenInfo* poOpenInfo) {
    char** papszParams = NULL;
    char* pszConnectionString = NULL;
    char* pszValidConnectionString = NULL;
    char* pszSchema = NULL;
    char* pszTable = NULL;
    char* pszColumn = NULL;
    char* pszWhere = NULL;
    char* pszTmp = NULL;
    int nMode = -1;
    int nPos = -1;
    int i = 0;
    PGconn * poConn = NULL;
    PostGISRasterDataset* poDS = NULL;
    GBool bBrowseDatabase = false;
    PGresult * poResult = NULL;
    CPLString osCommand;

    /**************************
     * Check input parameter
     **************************/
    if (poOpenInfo->pszFilename == NULL ||
            poOpenInfo->fp != NULL ||
            !EQUALN(poOpenInfo->pszFilename, "PG:", 3) ||
            (papszParams = ParseConnectionString(poOpenInfo->pszFilename)) == NULL) {
        /**
         * Drivers must quietly return NULL if the passed file is not of 
         * their format. They should only produce an error if the file 
         * does appear to be of their supported format, but for some 
         * reason, unsupported or corrupt
         */
        return NULL;
    }

    /**************************
     * Create driver instance
     **************************/
    /*
    poDriver = (PostGISRasterDriver*) GDALGetDriverByName("PostGISRaster");
    if (poDriver == NULL) {
        CPLError(CE_Failure, CPLE_AppDefined,
                "PostGIS Raster driver not registered");
        return NULL;
    }
    */

    /***************************************************************
     * Now check existence of db, schema, table, column and where.
     * If there's no enough data for querying one single table,
     * we'll create a GDAL metadata's list of sub-datasets, with all
     * the table with columns of type 'rasters'
     ***************************************************************/


    /**************************************************************************
     * Get mode:
     *  - 1. ONE_RASTER_PER_ROW: Each row is considered as a separate raster
     *  - 2. ONE_RASTER_PER_TABLE: All the table rows are considered as a whole
     *      raster coverage
     **************************************************************************/
    nPos = CSLFindName(papszParams, "mode");
    if (nPos != -1) {
        nMode = atoi(CPLParseNameValue(papszParams[nPos], NULL));
        
        if (nMode != ONE_RASTER_PER_ROW && nMode != ONE_RASTER_PER_TABLE) {
            /* Unrecognized mode, using default one */
            /*
            CPLError(CE_Warning, CPLE_AppDefined, "Undefined working mode (%d)."
                    " Valid working modes are 1 (ONE_RASTER_PER_ROW) and 2"
                    " (ONE_RASTER_PER_TABLE). Using ONE_RASTER_PER_TABLE"
                    " by default", nMode);
            */
            nMode = ONE_RASTER_PER_ROW;
        }

        /* Remove the mode from connection string */
        papszParams = CSLRemoveStrings(papszParams, nPos, 1, NULL);
    }

    /* Default mode */
    else
        nMode = ONE_RASTER_PER_ROW;

    /**
     * Case 1: There's no database name: Error, you need, at least,
     * specify a database name (NOTE: insensitive search)
     **/
    nPos = CSLFindName(papszParams, "dbname");
    if (nPos == -1) {
        CPLError(CE_Failure, CPLE_AppDefined,
                "You must specify at least a db name");
        return NULL;
    }

    /**
     * Case 2: There's database name, but no table name: activate a flag
     * for browsing the database, fetching all the schemas that contain
     * raster tables
     **/
    nPos = CSLFindName(papszParams, "table");
    if (nPos == -1) {
        bBrowseDatabase = true;

        /* Get schema name, if exist */
        nPos = CSLFindName(papszParams, "schema");
        if (nPos != -1) {
            pszSchema = CPLStrdup(CPLParseNameValue(papszParams[nPos], NULL));
            /* Delete this pair from params array */
            papszParams = CSLRemoveStrings(papszParams, nPos, 1, NULL);
        }

        /**
         * Remove the rest of the parameters, if exist (they mustn't be present
         * if we want a valid PQ connection string)
         **/
        nPos = CSLFindName(papszParams, "column");
        if (nPos != -1) {
            /* Delete this pair from params array */
            papszParams = CSLRemoveStrings(papszParams, nPos, 1, NULL);
        }

        nPos = CSLFindName(papszParams, "where");
        if (nPos != -1) {
            /* Delete this pair from params array */
            papszParams = CSLRemoveStrings(papszParams, nPos, 1, NULL);
        }
    }
    else {
        pszTable = CPLStrdup(CPLParseNameValue(papszParams[nPos], NULL));
        /* Delete this pair from params array */
        papszParams = CSLRemoveStrings(papszParams, nPos, 1, NULL);

        /**
         * Case 3: There's database and table name, but no column
         * name: Use a default column name and use the table to create the
         * dataset
         **/
        nPos = CSLFindName(papszParams, "column");
        if (nPos == -1) {
            pszColumn = CPLStrdup(DEFAULT_COLUMN);
        }
            /**
             * Case 4: There's database, table and column name: Use the table to
             * create a dataset
             **/
        else {
            pszColumn = CPLStrdup(CPLParseNameValue(papszParams[nPos], NULL));
            /* Delete this pair from params array */
            papszParams = CSLRemoveStrings(papszParams, nPos, 1, NULL);
        }

        /* Get the rest of the parameters, if exist */
        nPos = CSLFindName(papszParams, "schema");
        if (nPos == -1) {
            pszSchema = CPLStrdup(DEFAULT_SCHEMA);
        }
        else {
            pszSchema = CPLStrdup(CPLParseNameValue(papszParams[nPos], NULL));
            /* Delete this pair from params array */
            papszParams = CSLRemoveStrings(papszParams, nPos, 1, NULL);
        }

        nPos = CSLFindName(papszParams, "where");
        if (nPos != -1) {
            pszWhere = CPLStrdup(CPLParseNameValue(papszParams[nPos], NULL));
            /* Delete this pair from params array */
            papszParams = CSLRemoveStrings(papszParams, nPos, 1, NULL);
        }

    }

    /* Parse pszWhere, if needed */
    if (pszWhere) {
        pszTmp = ReplaceQuotes(pszWhere, strlen(pszWhere));
        CPLFree(pszWhere);
        pszWhere = pszTmp;
    }


    /***************************************
     * Construct a valid connection string
     ***************************************/
    pszValidConnectionString = (char*) CPLCalloc(strlen(poOpenInfo->pszFilename),
            sizeof (char));
    for (i = 0; i < CSLCount(papszParams); i++) {
        pszValidConnectionString = strncat(pszValidConnectionString, papszParams[i], strlen(papszParams[i]));
        pszValidConnectionString = strncat(pszValidConnectionString, " ", strlen(" "));

        //CPLStrlcat(pszValidConnectionString, papszParams[i], strlen(papszParams[i]));
        //CPLStrlcat(pszValidConnectionString, " ", strlen(" "));

    }



    /********************************************************************
     * Open a new database connection
     * TODO: Use enviroment vars (PGHOST, PGPORT, PGUSER) instead of
     * default values.
     * TODO: USE DRIVER INSTEAD OF OPENNING A CONNECTION HERE. MEMORY
     * PROBLEMS DETECTED (SEE DRIVER DESTRUCTOR FOR FURTHER INFORMATION)
     ********************************************************************/
    /*
    poConn = poDriver->GetConnection(pszValidConnectionString,
            CSLFetchNameValueDef(papszParams, "host", DEFAULT_HOST),
            CSLFetchNameValueDef(papszParams, "port", DEFAULT_PORT),
            CSLFetchNameValueDef(papszParams, "user", DEFAULT_USER),
            CSLFetchNameValueDef(papszParams, "password", DEFAULT_PASSWORD));

    if (poConn == NULL) {
        CPLError(CE_Failure, CPLE_AppDefined,
                "Couldn't establish a database connection");
        CSLDestroy(papszParams);
        CPLFree(pszConnectionString);
        CPLFree(pszValidConnectionString);
        if (pszSchema)
            CPLFree(pszSchema);
        if (pszTable)
            CPLFree(pszTable);
        if (pszColumn)
            CPLFree(pszColumn);
        if (pszWhere)
            CPLFree(pszWhere);

        delete poDriver;

        return NULL;
    }
    */


    /* Frees no longer needed memory */
    CSLDestroy(papszParams);
    CPLFree(pszConnectionString);

    /**
     * Get connection
     * TODO: Try to get connection from poDriver
     **/
    poConn = PQconnectdb(pszValidConnectionString);
    if (poConn == NULL)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                "Couldn't establish a database connection");        
        if (pszSchema)
            CPLFree(pszSchema);
        if (pszTable)
            CPLFree(pszTable);
        if (pszColumn)
            CPLFree(pszColumn);
        if (pszWhere)
            CPLFree(pszWhere);

        return NULL;
        
    }
    
    /* Check geometry type existence */
    poResult = PQexec(poConn, "SELECT oid FROM pg_type WHERE typname = 'geometry'");
    if (
            poResult == NULL ||
            PQresultStatus(poResult) != PGRES_TUPLES_OK ||
            PQntuples(poResult) <= 0
            ) {
        CPLError(CE_Failure, CPLE_AppDefined,
                "Error checking geometry type existence. Is PostGIS correctly \
                installed?: %s", PQerrorMessage(poConn));
        if (poResult != NULL)
            PQclear(poResult);
        if (pszSchema)
            CPLFree(pszSchema);
        if (pszTable)
            CPLFree(pszTable);
        if (pszColumn)
            CPLFree(pszColumn);
        if (pszWhere)
            CPLFree(pszWhere);
        
        PQfinish(poConn);

        //delete poDriver;

        return NULL;
    }

    PQclear(poResult);


    /* Check spatial tables existence */
    poResult = PQexec(poConn, "select pg_namespace.nspname as schemaname, \
					pg_class.relname as tablename from pg_class, \
					pg_namespace where pg_class.relnamespace = pg_namespace.oid \
					and (pg_class.relname='raster_columns' or \
                    pg_class.relname='raster_overviews' or \
					pg_class.relname='geometry_columns' or \
					pg_class.relname='spatial_ref_sys')");
    if (
            poResult == NULL ||
            PQresultStatus(poResult) != PGRES_TUPLES_OK ||
            PQntuples(poResult) <= 0
            ) {
        CPLError(CE_Failure, CPLE_AppDefined,
                "Error checking needed tables existence: %s",
                PQerrorMessage(poConn));
        if (poResult != NULL)
            PQclear(poResult);
        if (pszSchema)
            CPLFree(pszSchema);
        if (pszTable)
            CPLFree(pszTable);
        if (pszColumn)
            CPLFree(pszColumn);
        if (pszWhere)
            CPLFree(pszWhere);

        PQfinish(poConn);

        //delete poDriver;

        return NULL;

    }

    PQclear(poResult);

    /*************************************************************************
     * No table will be read. Only shows information about the existent raster
     * tables
     *************************************************************************/
    if (bBrowseDatabase) {
        /**
         * Creates empty dataset object, only for subdatasets
         **/
        poDS = new PostGISRasterDataset();
        poDS->poConn = poConn;
        poDS->eAccess = GA_ReadOnly;
        //poDS->poDriver = poDriver;
        poDS->nMode = (pszSchema) ? BROWSE_SCHEMA : BROWSE_DATABASE;
        poDS->nRasterXSize = 0;
        poDS->nRasterYSize = 0;
        poDS->adfGeoTransform[0] = 0.0;
        poDS->adfGeoTransform[1] = 0.0;
        poDS->adfGeoTransform[2] = 0.0;
        poDS->adfGeoTransform[3] = 0.0;
        poDS->adfGeoTransform[4] = 0.0;
        poDS->adfGeoTransform[5] = 0.0;

        /**
         * Look for raster tables at database and
         * store them as subdatasets
         **/
        if (!poDS->BrowseDatabase(pszSchema, pszValidConnectionString)) {
            CPLFree(pszValidConnectionString);
            delete poDS;
            return NULL;
        }
    }
        /***********************************************************************
         * A table will be read: Fetch raster properties from db. Pay attention
         * to the block size: if the raster is blocked at database, the block
         * size can be fetched from each block size, if regular blocking table
         **********************************************************************/
    else {
        poDS = new PostGISRasterDataset();
        poDS->poConn = poConn;
        poDS->eAccess = poOpenInfo->eAccess;
        poDS->nMode = nMode;
        //poDS->poDriver = poDriver;

        poDS->pszSchema = pszSchema;
        poDS->pszTable = pszTable;
        poDS->pszColumn = pszColumn;
        poDS->pszWhere = pszWhere;

        /* Fetch basic raster metadata from db */
        if (!poDS->SetRasterProperties(pszValidConnectionString)) {
            CPLFree(pszValidConnectionString);
            delete poDS;
            return NULL;
        }

        /* Set raster bands */
        if (!poDS->SetRasterBands()) {
            CPLFree(pszValidConnectionString);
            delete poDS;
            return NULL;
        }
        
    }

    CPLFree(pszValidConnectionString);
    return poDS;

}

/*****************************************
 * \brief Get Metadata from raster
 * TODO: Add more options (the result of
 * calling ST_Metadata, for example)
 *****************************************/
char** PostGISRasterDataset::GetMetadata(const char *pszDomain) {
    if (pszDomain != NULL && EQUALN(pszDomain, "SUBDATASETS", 11))
        return papszSubdatasets;
    else
        return GDALDataset::GetMetadata(pszDomain);
}

/*****************************************************
 * \brief Fetch the projection definition string 
 * for this dataset in OpenGIS WKT format. It should 
 * be suitable for use with the OGRSpatialReference 
 * class.
 *****************************************************/
const char* PostGISRasterDataset::GetProjectionRef() {
    CPLString osCommand;
    PGresult* poResult;

    if (nSrid == -1)
        return "";

    if (pszProjection)
        return pszProjection;

    /********************************************************
     *          Reading proj from database
     ********************************************************/
    osCommand.Printf("SELECT srtext FROM spatial_ref_sys where SRID=%d",
            nSrid);
    poResult = PQexec(this->poConn, osCommand.c_str());
    if (poResult && PQresultStatus(poResult) == PGRES_TUPLES_OK
            && PQntuples(poResult) > 0) {
        /*
         * TODO: Memory leak detected with valgrind caused by allocation here.
         * Even when the return string is freed
         */
        pszProjection = CPLStrdup(PQgetvalue(poResult, 0, 0));
    }

    if (poResult)
        PQclear(poResult);

    return pszProjection;
}

/**********************************************************
 * \brief Set projection definition. The input string must
 * be in OGC WKT or PROJ.4 format
 **********************************************************/
CPLErr PostGISRasterDataset::SetProjection(const char * pszProjectionRef)
{
    VALIDATE_POINTER1(pszProjectionRef, "SetProjection", CE_Failure);

    CPLString osCommand;
    PGresult * poResult;
    int nFetchedSrid = -1;


    /*****************************************************************
     * Check if the dataset allows updating
     *****************************************************************/
    if (GetAccess() != GA_Update) {
        CPLError(CE_Failure, CPLE_NoWriteAccess,
                "This driver doesn't allow write access");
        return CE_Failure;
    }

    /*****************************************************************
     * Look for projection with this text
     *****************************************************************/

    // First, WKT text
    osCommand.Printf("SELECT srid FROM spatial_ref_sys where srtext='%s'",
            pszProjectionRef);
    poResult = PQexec(poConn, osCommand.c_str());

    if (poResult && PQresultStatus(poResult) == PGRES_TUPLES_OK
            && PQntuples(poResult) > 0) {

        nFetchedSrid = atoi(PQgetvalue(poResult, 0, 0));

        // update class attribute
        nSrid = nFetchedSrid;


        // update raster_columns table
        osCommand.Printf("UPDATE raster_columns SET srid=%d WHERE \
                    r_table_name = '%s' AND r_column = '%s'",
                nSrid, pszTable, pszColumn);
        poResult = PQexec(poConn, osCommand.c_str());
        if (poResult == NULL || PQresultStatus(poResult) != PGRES_COMMAND_OK) {
            CPLError(CE_Failure, CPLE_AppDefined,
                    "Couldn't update raster_columns table: %s",
                    PQerrorMessage(poConn));
            return CE_Failure;
        }

        // TODO: Update ALL blocks with the new srid...

        return CE_None;
    }

    // If not, proj4 text
    else {
        osCommand.Printf(
                "SELECT srid FROM spatial_ref_sys where proj4text='%s'",
            pszProjectionRef);
        poResult = PQexec(poConn, osCommand.c_str());

        if (poResult && PQresultStatus(poResult) == PGRES_TUPLES_OK
            && PQntuples(poResult) > 0) {

            nFetchedSrid = atoi(PQgetvalue(poResult, 0, 0));

            // update class attribute
            nSrid = nFetchedSrid;

            // update raster_columns table            
            osCommand.Printf("UPDATE raster_columns SET srid=%d WHERE \
                    r_table_name = '%s' AND r_column = '%s'",
                    nSrid, pszTable, pszColumn);

            poResult = PQexec(poConn, osCommand.c_str());
            if (poResult == NULL ||
                    PQresultStatus(poResult) != PGRES_COMMAND_OK) {
                CPLError(CE_Failure, CPLE_AppDefined,
                        "Couldn't update raster_columns table: %s",
                        PQerrorMessage(poConn));
                return CE_Failure;
            }

            // TODO: Update ALL blocks with the new srid...

            return CE_None;
        }

        else {
            CPLError(CE_Failure, CPLE_WrongFormat,
                    "Couldn't find WKT neither proj4 definition");
            return CE_Failure;
        }
    }
}

/********************************************************
 * \brief Set the affine transformation coefficients
 ********************************************************/
CPLErr PostGISRasterDataset::SetGeoTransform(double* padfTransform) {
    if (!padfTransform)
        return CE_Failure;

    adfGeoTransform[0] = padfTransform[0];
    adfGeoTransform[1] = padfTransform[1];
    adfGeoTransform[2] = padfTransform[2];
    adfGeoTransform[3] = padfTransform[3];
    adfGeoTransform[4] = padfTransform[4];
    adfGeoTransform[5] = padfTransform[5];

    return CE_None;
}

/********************************************************
 * \brief Get the affine transformation coefficients
 ********************************************************/
CPLErr PostGISRasterDataset::GetGeoTransform(double * padfTransform) {
    // copy necessary values in supplied buffer
    padfTransform[0] = adfGeoTransform[0];
    padfTransform[1] = adfGeoTransform[1];
    padfTransform[2] = adfGeoTransform[2];
    padfTransform[3] = adfGeoTransform[3];
    padfTransform[4] = adfGeoTransform[4];
    padfTransform[5] = adfGeoTransform[5];

    return CE_None;
}


/************************************************************************/
/*                          GDALRegister_PostGISRaster()                    */
/************************************************************************/
void GDALRegister_PostGISRaster() {
    GDALDriver *poDriver;

    if (GDALGetDriverByName("PostGISRaster") == NULL) {
        poDriver = new GDALDriver();

        poDriver->SetDescription("PostGISRaster");
        poDriver->SetMetadataItem(GDAL_DMD_LONGNAME,
                "PostGIS Raster driver");

        poDriver->pfnOpen = PostGISRasterDataset::Open;

        GetGDALDriverManager()->RegisterDriver(poDriver);
    }
}

