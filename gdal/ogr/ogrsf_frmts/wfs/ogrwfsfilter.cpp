/******************************************************************************
 * $Id$
 *
 * Project:  WFS Translator
 * Purpose:  Implements OGR SQL into OGC Filter translation.
 * Author:   Even Rouault, <even dot rouault at mines dash paris dot org>
 *
 ******************************************************************************
 * Copyright (c) 2010-2012, Even Rouault <even dot rouault at mines-paris dot org>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 ****************************************************************************/

#include "ogr_wfs.h"
#include "ogr_p.h"

CPL_CVSID("$Id$");

typedef struct
{
    int nVersion;
    int bPropertyIsNotEqualToSupported;
    int bOutNeedsNullCheck;
    OGRFeatureDefn* poFDefn;
    int nUniqueGeomGMLId;
} ExprDumpFilterOptions;

/************************************************************************/
/*                WFS_ExprDumpGmlObjectIdFilter()                       */
/************************************************************************/

static int WFS_ExprDumpGmlObjectIdFilter(CPLString& osFilter,
                                         const swq_expr_node* poExpr,
                                         int bUseFeatureId,
                                         int bGmlObjectIdNeedsGMLPrefix,
                                         int nVersion)
{
    if (poExpr->eNodeType == SNT_OPERATION &&
        poExpr->nOperation == SWQ_EQ &&
        poExpr->nSubExprCount == 2 &&
        poExpr->papoSubExpr[0]->eNodeType == SNT_COLUMN &&
        strcmp(poExpr->papoSubExpr[0]->string_value, "gml_id") == 0 &&
        poExpr->papoSubExpr[1]->eNodeType == SNT_CONSTANT)
    {
        if (bUseFeatureId)
            osFilter += "<FeatureId fid=\"";
        else if (nVersion >= 200)
            osFilter += "<ResourceId rid=\"";
        else if (!bGmlObjectIdNeedsGMLPrefix)
            osFilter += "<GmlObjectId id=\"";
        else
            osFilter += "<GmlObjectId gml:id=\"";
        if( poExpr->papoSubExpr[1]->field_type == SWQ_INTEGER ||
            poExpr->papoSubExpr[1]->field_type == SWQ_INTEGER64 )
            osFilter += CPLSPrintf(CPL_FRMT_GIB, poExpr->papoSubExpr[1]->int_value);
        else if( poExpr->papoSubExpr[1]->field_type == SWQ_STRING )
        {
            char* pszXML = CPLEscapeString(poExpr->papoSubExpr[1]->string_value, -1, CPLES_XML);
            osFilter += pszXML;
            CPLFree(pszXML);
        }
        else
            return FALSE;
        osFilter += "\"/>";
        return TRUE;
    }
    else if (poExpr->eNodeType == SNT_OPERATION &&
             poExpr->nOperation == SWQ_OR &&
             poExpr->nSubExprCount == 2 )
    {
        return WFS_ExprDumpGmlObjectIdFilter(osFilter, poExpr->papoSubExpr[0],
                                             bUseFeatureId, bGmlObjectIdNeedsGMLPrefix, nVersion) &&
               WFS_ExprDumpGmlObjectIdFilter(osFilter, poExpr->papoSubExpr[1],
                                             bUseFeatureId, bGmlObjectIdNeedsGMLPrefix, nVersion);
    }
    return FALSE;
}

/************************************************************************/
/*                     WFS_ExprDumpRawLitteral()                        */
/************************************************************************/

static int WFS_ExprDumpRawLitteral(CPLString& osFilter,
                                   const swq_expr_node* poExpr)
{
    if( poExpr->field_type == SWQ_INTEGER ||
        poExpr->field_type == SWQ_INTEGER64 )
        osFilter += CPLSPrintf(CPL_FRMT_GIB, poExpr->int_value);
    else if( poExpr->field_type == SWQ_FLOAT )
        osFilter += CPLSPrintf("%.16g", poExpr->float_value);
    else if( poExpr->field_type == SWQ_STRING )
    {
        char* pszXML = CPLEscapeString(poExpr->string_value, -1, CPLES_XML);
        osFilter += pszXML;
        CPLFree(pszXML);
    }
    else if( poExpr->field_type == SWQ_TIMESTAMP )
    {
        OGRField sDate;
        if( !OGRParseDate(poExpr->string_value, &sDate, 0) )
            return FALSE;
        char* pszDate = OGRGetXMLDateTime(&sDate);
        osFilter += pszDate;
        CPLFree(pszDate);
    }
    else
        return FALSE;
    return TRUE;
}

/************************************************************************/
/*                       WFS_ExprGetSRSName()                          */
/************************************************************************/

static const char* WFS_ExprGetSRSName( const swq_expr_node* poExpr,
                                       int iSubArgIndex,
                                       ExprDumpFilterOptions* psOptions,
                                       OGRSpatialReference& oSRS )
{
    if( poExpr->nSubExprCount == iSubArgIndex + 1 &&
        poExpr->papoSubExpr[iSubArgIndex]->field_type == SWQ_STRING )
    {
        if( oSRS.SetFromUserInput(poExpr->papoSubExpr[iSubArgIndex]->string_value) == OGRERR_NONE )
        {
            return poExpr->papoSubExpr[iSubArgIndex]->string_value;
        }
    }
    else if ( poExpr->nSubExprCount == iSubArgIndex + 1 &&
              poExpr->papoSubExpr[iSubArgIndex]->field_type == SWQ_INTEGER )
    {
        if( oSRS.importFromEPSGA(poExpr->papoSubExpr[iSubArgIndex]->int_value) == OGRERR_NONE )
        {
            return CPLSPrintf("urn:ogc:def:crs:EPSG::%d", (int)poExpr->papoSubExpr[iSubArgIndex]->int_value);
        }
    }
    else if( poExpr->nSubExprCount == iSubArgIndex &&
             psOptions->poFDefn->GetGeomFieldCount() > 0 )
    {
        OGRSpatialReference* poSRS = psOptions->poFDefn->GetGeomFieldDefn(0)->GetSpatialRef();
        if( poSRS &&
            poSRS->GetAuthorityName(NULL) &&
            EQUAL(poSRS->GetAuthorityName(NULL), "EPSG") &&
            poSRS->GetAuthorityCode(NULL) &&
            oSRS.importFromEPSGA(atoi(poSRS->GetAuthorityCode(NULL))) == OGRERR_NONE )
        {
            return CPLSPrintf("urn:ogc:def:crs:EPSG::%s", poSRS->GetAuthorityCode(NULL));
        }
    }
    return NULL;
}

/************************************************************************/
/*                     WFS_ExprDumpAsOGCFilter()                        */
/************************************************************************/

static int WFS_ExprDumpAsOGCFilter(CPLString& osFilter,
                                   const swq_expr_node* poExpr,
                                   int bExpectBinary,
                                   ExprDumpFilterOptions* psOptions)
{
    if( poExpr->eNodeType == SNT_COLUMN )
    {
        if (bExpectBinary)
            return FALSE;

        /* Special fields not understood by server */
        if (EQUAL(poExpr->string_value, "gml_id") ||
            EQUAL(poExpr->string_value, "FID") ||
            EQUAL(poExpr->string_value, "OGR_GEOMETRY") ||
            EQUAL(poExpr->string_value, "OGR_GEOM_WKT") ||
            EQUAL(poExpr->string_value, "OGR_GEOM_AREA") ||
            EQUAL(poExpr->string_value, "OGR_STYLE"))
        {
            CPLDebug("WFS", "Attribute refers to a OGR special field. Cannot use server-side filtering");
            return FALSE;
        }

        const char* pszFieldname = poExpr->string_value;
        int nIndex;
        if( (nIndex = psOptions->poFDefn->GetFieldIndex(pszFieldname)) >= 0 )
        {
            pszFieldname = psOptions->poFDefn->GetFieldDefn(nIndex)->GetNameRef();
        }
        else if( (nIndex = psOptions->poFDefn->GetGeomFieldIndex(pszFieldname)) >= 0 )
        {
            pszFieldname = psOptions->poFDefn->GetGeomFieldDefn(nIndex)->GetNameRef();
        }
        else
        {
            CPLDebug("WFS", "Field '%s' unknown. Cannot use server-side filtering",
                        pszFieldname);
            return FALSE;
        }

        if (psOptions->nVersion >= 200)
            osFilter += "<ValueReference>";
        else
            osFilter += "<PropertyName>";
        char* pszFieldnameXML = CPLEscapeString(pszFieldname, -1, CPLES_XML);
        osFilter += pszFieldnameXML;
        CPLFree(pszFieldnameXML);
        if (psOptions->nVersion >= 200)
            osFilter += "</ValueReference>";
        else
            osFilter += "</PropertyName>";

        return TRUE;
    }

    if( poExpr->eNodeType == SNT_CONSTANT )
    {
        if (bExpectBinary)
            return FALSE;

        osFilter += "<Literal>";
        if( !WFS_ExprDumpRawLitteral(osFilter, poExpr) )
            return FALSE;
        osFilter += "</Literal>";

        return TRUE;
    }
    
    if( poExpr->eNodeType != SNT_OPERATION )
        return FALSE; /* shouldn't happen */

    if( poExpr->nOperation == SWQ_NOT )
    {
        osFilter += "<Not>";
        if (!WFS_ExprDumpAsOGCFilter(osFilter, poExpr->papoSubExpr[0], TRUE, psOptions))
            return FALSE;
        osFilter += "</Not>";
        return TRUE;
    }

    if( poExpr->nOperation == SWQ_LIKE )
    {
        CPLString osVal;
        char ch;
        char firstCh = 0;
        int i;
        if (psOptions->nVersion == 100)
            osFilter += "<PropertyIsLike wildCard='*' singleChar='_' escape='!'>";
        else
            osFilter += "<PropertyIsLike wildCard='*' singleChar='_' escapeChar='!'>";
        if (!WFS_ExprDumpAsOGCFilter(osFilter, poExpr->papoSubExpr[0], FALSE, psOptions))
            return FALSE;
        if (poExpr->papoSubExpr[1]->eNodeType != SNT_CONSTANT &&
            poExpr->papoSubExpr[1]->field_type != SWQ_STRING)
            return FALSE;
        osFilter += "<Literal>";

        /* Escape value according to above special characters */
        /* For URL compatibility reason, we remap the OGR SQL '%' wildchard into '*' */
        i = 0;
        ch = poExpr->papoSubExpr[1]->string_value[i];
        if (ch == '\'' || ch == '"')
        {
            firstCh = ch;
            i ++;
        }
        for(;(ch = poExpr->papoSubExpr[1]->string_value[i]) != '\0';i++)
        {
            if (ch == '%')
                osVal += "*";
            else if (ch == '!')
                osVal += "!!";
            else if (ch == '*')
                osVal += "!*";
            else if (ch == firstCh && poExpr->papoSubExpr[1]->string_value[i + 1] == 0)
                break;
            else
            {
                char ach[2];
                ach[0] = ch;
                ach[1] = 0;
                osVal += ach;
            }
        }
        char* pszXML = CPLEscapeString(osVal, -1, CPLES_XML);
        osFilter += pszXML;
        CPLFree(pszXML);
        osFilter += "</Literal>";
        osFilter += "</PropertyIsLike>";
        return TRUE;
    }

    if( poExpr->nOperation == SWQ_ISNULL )
    {
        osFilter += "<PropertyIsNull>";
        if (!WFS_ExprDumpAsOGCFilter(osFilter, poExpr->papoSubExpr[0], FALSE, psOptions))
            return FALSE;
        osFilter += "</PropertyIsNull>";
        psOptions->bOutNeedsNullCheck = TRUE;
        return TRUE;
    }

    if( poExpr->nOperation == SWQ_EQ ||
        poExpr->nOperation == SWQ_NE ||
        poExpr->nOperation == SWQ_LE ||
        poExpr->nOperation == SWQ_LT ||
        poExpr->nOperation == SWQ_GE ||
        poExpr->nOperation == SWQ_GT )
    {
        int nOperation = poExpr->nOperation;
        int bAddClosingNot = FALSE;
        if (!psOptions->bPropertyIsNotEqualToSupported && nOperation == SWQ_NE)
        {
            osFilter += "<Not>";
            nOperation = SWQ_EQ;
            bAddClosingNot = TRUE;
        }

        const char* pszName = NULL;
        switch(nOperation)
        {
            case SWQ_EQ:  pszName = "PropertyIsEqualTo"; break;
            case SWQ_NE:  pszName = "PropertyIsNotEqualTo"; break;
            case SWQ_LE:  pszName = "PropertyIsLessThanOrEqualTo"; break;
            case SWQ_LT:  pszName = "PropertyIsLessThan"; break;
            case SWQ_GE:  pszName = "PropertyIsGreaterThanOrEqualTo"; break;
            case SWQ_GT:  pszName = "PropertyIsGreaterThan"; break;
            default: break;
        }
        osFilter += "<";
        osFilter += pszName;
        osFilter += ">";
        if (!WFS_ExprDumpAsOGCFilter(osFilter, poExpr->papoSubExpr[0], FALSE, psOptions))
            return FALSE;
        if (!WFS_ExprDumpAsOGCFilter(osFilter, poExpr->papoSubExpr[1], FALSE, psOptions))
            return FALSE;
        osFilter += "</";
        osFilter += pszName;
        osFilter += ">";
        if (bAddClosingNot)
            osFilter += "</Not>";
        return TRUE;
    }

    if( poExpr->nOperation == SWQ_AND ||
        poExpr->nOperation == SWQ_OR )
    {
        const char* pszName = (poExpr->nOperation == SWQ_AND) ? "And" : "Or";
        osFilter += "<";
        osFilter += pszName;
        osFilter += ">";
        if (!WFS_ExprDumpAsOGCFilter(osFilter, poExpr->papoSubExpr[0], TRUE, psOptions))
            return FALSE;
        if (!WFS_ExprDumpAsOGCFilter(osFilter, poExpr->papoSubExpr[1], TRUE, psOptions))
            return FALSE;
        osFilter += "</";
        osFilter += pszName;
        osFilter += ">";
        return TRUE;
    }
    
    if( poExpr->nOperation == SWQ_CUSTOM_FUNC &&
        EQUAL(poExpr->string_value, "ST_MakeEnvelope") )
    {
        OGRSpatialReference oSRS;
        const char* pszSRSName = WFS_ExprGetSRSName( poExpr, 4, psOptions, oSRS );
        int bAxisSwap = FALSE;

        osFilter += "<gml:Envelope";
        if( pszSRSName )
        {
            osFilter += " srsName=\"";
            osFilter += pszSRSName;
            osFilter += "\"";
            if( oSRS.EPSGTreatsAsLatLong() || oSRS.EPSGTreatsAsNorthingEasting() )
                bAxisSwap = TRUE;
        }
        osFilter += ">";
        osFilter += "<gml:lowerCorner>";
        if (!WFS_ExprDumpRawLitteral(osFilter, poExpr->papoSubExpr[(bAxisSwap) ? 1 : 0]))
            return FALSE;
        osFilter += " ";
        if (!WFS_ExprDumpRawLitteral(osFilter, poExpr->papoSubExpr[(bAxisSwap) ? 0 : 1]))
            return FALSE;
        osFilter += "</gml:lowerCorner>";
        osFilter += "<gml:upperCorner>";
        if (!WFS_ExprDumpRawLitteral(osFilter, poExpr->papoSubExpr[(bAxisSwap) ? 3 : 2]))
            return FALSE;
        osFilter += " ";
        if (!WFS_ExprDumpRawLitteral(osFilter, poExpr->papoSubExpr[(bAxisSwap) ? 2 : 3]))
            return FALSE;
        osFilter += "</gml:upperCorner>";
        osFilter += "</gml:Envelope>";
        return TRUE;
    }

    if( poExpr->nOperation == SWQ_CUSTOM_FUNC &&
        EQUAL(poExpr->string_value, "ST_GeomFromText") )
    {
        OGRSpatialReference oSRS;
        const char* pszSRSName = WFS_ExprGetSRSName( poExpr, 1, psOptions, oSRS );
        OGRGeometry* poGeom = NULL;
        char* pszWKT = (char*)poExpr->papoSubExpr[0]->string_value;
        OGRGeometryFactory::createFromWkt(&pszWKT, NULL, &poGeom);
        char** papszOptions = NULL;
        papszOptions = CSLSetNameValue(papszOptions, "FORMAT", "GML3");
        if( pszSRSName != NULL )
        {
            if( oSRS.EPSGTreatsAsLatLong() || oSRS.EPSGTreatsAsNorthingEasting() )
            {
                OGR_SRSNode *poGEOGCS = oSRS.GetAttrNode( "GEOGCS" );
                if( poGEOGCS != NULL )
                    poGEOGCS->StripNodes( "AXIS" );

                OGR_SRSNode *poPROJCS = oSRS.GetAttrNode( "PROJCS" );
                if (poPROJCS != NULL && oSRS.EPSGTreatsAsNorthingEasting())
                    poPROJCS->StripNodes( "AXIS" );

                if( EQUALN(pszSRSName, "urn:ogc:def:crs:EPSG::", strlen("urn:ogc:def:crs:EPSG::")) )
                    papszOptions = CSLSetNameValue(papszOptions, "GML3_LONGSRS", "YES");
            }

            poGeom->assignSpatialReference(&oSRS);
        }
        papszOptions = CSLSetNameValue(papszOptions, "GMLID",
                                       CPLSPrintf("id%d", psOptions->nUniqueGeomGMLId ++));
        char* pszGML = OGR_G_ExportToGMLEx( (OGRGeometryH)poGeom, papszOptions );
        osFilter += pszGML;
        CSLDestroy(papszOptions);
        delete poGeom;
        CPLFree(pszGML);
        return TRUE;
    }

    if( poExpr->nOperation == SWQ_CUSTOM_FUNC )
    {
        const char* pszName =
            EQUAL(poExpr->string_value, "ST_Equals") ? "Equals" :
            EQUAL(poExpr->string_value, "ST_Disjoint") ? "Disjoint" :
            EQUAL(poExpr->string_value, "ST_Touches") ? "Touches" :
            EQUAL(poExpr->string_value, "ST_Contains") ? "Contains" :
            EQUAL(poExpr->string_value, "ST_Intersects") ? "Intersects" :
            EQUAL(poExpr->string_value, "ST_Within") ? "Within" :
            EQUAL(poExpr->string_value, "ST_Crosses") ? "Crosses" :
            EQUAL(poExpr->string_value, "ST_Overlaps") ? "Overlaps" :
            NULL;
        if( pszName == NULL )
            return FALSE;
        osFilter += "<";
        osFilter += pszName;
        osFilter += ">";
        for(int i=0;i<poExpr->nSubExprCount;i++)
        {
            if (!WFS_ExprDumpAsOGCFilter(osFilter, poExpr->papoSubExpr[i], FALSE, psOptions))
                return FALSE;
        }
        osFilter += "</";
        osFilter += pszName;
        osFilter += ">";
        return TRUE;
    }

    return FALSE;
}

/************************************************************************/
/*               WFS_TurnSQLFilterToOGCFilter()                         */
/************************************************************************/

CPLString WFS_TurnSQLFilterToOGCFilter( const swq_expr_node* poExpr,
                                        OGRFeatureDefn* poFDefn,
                                        int nVersion,
                                        int bPropertyIsNotEqualToSupported,
                                        int bUseFeatureId,
                                        int bGmlObjectIdNeedsGMLPrefix,
                                        int* pbOutNeedsNullCheck )
{
    CPLString osFilter;
    /* If the filter is only made of querying one or several gml_id */
    /* (with OR operator), we turn this to <GmlObjectId> list */
    if (!WFS_ExprDumpGmlObjectIdFilter(osFilter, poExpr, bUseFeatureId,
                                       bGmlObjectIdNeedsGMLPrefix, nVersion))
    {
        ExprDumpFilterOptions sOptions;
        sOptions.nVersion = nVersion;
        sOptions.bPropertyIsNotEqualToSupported = bPropertyIsNotEqualToSupported;
        sOptions.bOutNeedsNullCheck = FALSE;
        sOptions.poFDefn = poFDefn;
        sOptions.nUniqueGeomGMLId = 1;
        osFilter = "";
        if (!WFS_ExprDumpAsOGCFilter(osFilter, poExpr, TRUE, &sOptions))
            osFilter = "";
        /*else
            CPLDebug("WFS", "Filter %s", osFilter.c_str());*/
        *pbOutNeedsNullCheck = sOptions.bOutNeedsNullCheck;
    }

    return osFilter;
}

/************************************************************************/
/*                  OGRWFSSpatialBooleanPredicateChecker()              */
/************************************************************************/

static swq_field_type OGRWFSSpatialBooleanPredicateChecker( swq_expr_node *op,
                                               CPL_UNUSED int bAllowMismatchTypeOnFieldComparison )
{
    if( op->nSubExprCount != 2 )
    {
        CPLError(CE_Failure, CPLE_AppDefined, "Wrong number of arguments for %s",
                 op->string_value);
        return SWQ_ERROR;
    }
    for(int i=0;i<op->nSubExprCount;i++)
    {
        if( op->papoSubExpr[i]->field_type != SWQ_GEOMETRY )
        {
            CPLError(CE_Failure, CPLE_AppDefined, "Wrong field type for argument %d of %s",
                     i+1, op->string_value);
            return SWQ_ERROR;
        }
    }
    return SWQ_BOOLEAN;
}

/************************************************************************/
/*                           OGRWFSCheckSRIDArg()                       */
/************************************************************************/

static int OGRWFSCheckSRIDArg( swq_expr_node *op, int iSubArgIndex ) 
{
    if( op->papoSubExpr[iSubArgIndex]->field_type == SWQ_INTEGER )
    {
        OGRSpatialReference oSRS;
        if( oSRS.importFromEPSGA(op->papoSubExpr[iSubArgIndex]->int_value) != OGRERR_NONE )
        {
            CPLError(CE_Failure, CPLE_AppDefined, "Wrong value for argument %d of %s",
                     iSubArgIndex + 1, op->string_value);
            return FALSE;
        }
    }
    else if( op->papoSubExpr[iSubArgIndex]->field_type == SWQ_STRING )
    {
        OGRSpatialReference oSRS;
        if( oSRS.SetFromUserInput(op->papoSubExpr[iSubArgIndex]->string_value) != OGRERR_NONE )
        {
            CPLError(CE_Failure, CPLE_AppDefined, "Wrong value for argument %d of %s",
                     iSubArgIndex + 1, op->string_value);
            return FALSE;
        }
    }
    else
    {
        CPLError(CE_Failure, CPLE_AppDefined, "Wrong field type for argument %d of %s",
                 iSubArgIndex + 1, op->string_value);
        return FALSE;
    }
    return TRUE;
}

/************************************************************************/
/*                   OGRWFSMakeEnvelopeChecker()                        */
/************************************************************************/

static swq_field_type OGRWFSMakeEnvelopeChecker( swq_expr_node *op,
                                                 CPL_UNUSED int bAllowMismatchTypeOnFieldComparison )
{
    if( op->nSubExprCount != 4 && op->nSubExprCount != 5 )
    {
        CPLError(CE_Failure, CPLE_AppDefined, "Wrong number of arguments for %s",
                 op->string_value);
        return SWQ_ERROR;
    }
    for(int i=0;i<4;i++)
    {
        if( op->papoSubExpr[i]->field_type != SWQ_INTEGER &&
            op->papoSubExpr[i]->field_type != SWQ_INTEGER64 &&
            op->papoSubExpr[i]->field_type != SWQ_FLOAT )
        {
            CPLError(CE_Failure, CPLE_AppDefined, "Wrong field type for argument %d of %s",
                     i+1, op->string_value);
            return SWQ_ERROR;
        }
    }
    if( op->nSubExprCount == 5 )
    {
        if( !OGRWFSCheckSRIDArg( op, 4 ) )
            return SWQ_ERROR;
    }
    return SWQ_GEOMETRY;
}

/************************************************************************/
/*                   OGRWFSGeomFromTextChecker()                        */
/************************************************************************/

static swq_field_type OGRWFSGeomFromTextChecker( swq_expr_node *op,
                                                 CPL_UNUSED int bAllowMismatchTypeOnFieldComparison )
{
    if( op->nSubExprCount != 1&& op->nSubExprCount != 2 )
    {
        CPLError(CE_Failure, CPLE_AppDefined, "Wrong number of arguments for %s",
                 op->string_value);
        return SWQ_ERROR;
    }
    if( op->papoSubExpr[0]->field_type != SWQ_STRING )
    {
        CPLError(CE_Failure, CPLE_AppDefined, "Wrong field type for argument %d of %s",
                    1, op->string_value);
        return SWQ_ERROR;
    }
    OGRGeometry* poGeom = NULL;
    char* pszWKT = (char*)op->papoSubExpr[0]->string_value;
    if( OGRGeometryFactory::createFromWkt(&pszWKT, NULL, &poGeom) != OGRERR_NONE )
    {
        CPLError(CE_Failure, CPLE_AppDefined, "Wrong value for argument %d of %s",
                  1, op->string_value);
        return SWQ_ERROR;
    }
    delete poGeom;
    if( op->nSubExprCount == 2 )
    {
        if( !OGRWFSCheckSRIDArg( op, 1 ) )
            return SWQ_ERROR;
    }
    return SWQ_GEOMETRY;
}

/************************************************************************/
/*                   OGRWFSCustomFuncRegistrar                          */
/************************************************************************/

class OGRWFSCustomFuncRegistrar: public swq_custom_func_registrar
{
    public:
        OGRWFSCustomFuncRegistrar() {};
        virtual const swq_operation *GetOperator( const char * ) ;
};

/************************************************************************/
/*                  WFSGetCustomFuncRegistrar()                         */
/************************************************************************/

swq_custom_func_registrar* WFSGetCustomFuncRegistrar()
{
    static OGRWFSCustomFuncRegistrar obj;
    return &obj;
}

/************************************************************************/
/*                           GetOperator()                              */
/************************************************************************/

static const swq_operation OGRWFSSpatialOps[] = {
{ "ST_Equals", SWQ_CUSTOM_FUNC, NULL, OGRWFSSpatialBooleanPredicateChecker },
{ "ST_Disjoint", SWQ_CUSTOM_FUNC, NULL, OGRWFSSpatialBooleanPredicateChecker },
{ "ST_Touches", SWQ_CUSTOM_FUNC, NULL, OGRWFSSpatialBooleanPredicateChecker },
{ "ST_Contains", SWQ_CUSTOM_FUNC, NULL, OGRWFSSpatialBooleanPredicateChecker },
/*{ "ST_Covers", SWQ_CUSTOM_FUNC, NULL, OGRWFSSpatialBooleanPredicateChecker },*/
{ "ST_Intersects", SWQ_CUSTOM_FUNC, NULL, OGRWFSSpatialBooleanPredicateChecker },
{ "ST_Within", SWQ_CUSTOM_FUNC, NULL, OGRWFSSpatialBooleanPredicateChecker },
/*{ "ST_CoveredBy", SWQ_CUSTOM_FUNC, NULL, OGRWFSSpatialBooleanPredicateChecker },*/
{ "ST_Crosses", SWQ_CUSTOM_FUNC, NULL, OGRWFSSpatialBooleanPredicateChecker },
{ "ST_Overlaps", SWQ_CUSTOM_FUNC, NULL, OGRWFSSpatialBooleanPredicateChecker },
{ "ST_MakeEnvelope", SWQ_CUSTOM_FUNC, NULL, OGRWFSMakeEnvelopeChecker },
{ "ST_GeomFromText", SWQ_CUSTOM_FUNC, NULL, OGRWFSGeomFromTextChecker }
};

const swq_operation *OGRWFSCustomFuncRegistrar::GetOperator( const char * pszName )
{
    for(int i=0; i< (int)(sizeof(OGRWFSSpatialOps)/sizeof(OGRWFSSpatialOps[0]));i++)
    {
        if( EQUAL(OGRWFSSpatialOps[i].pszName, pszName) )
            return &OGRWFSSpatialOps[i];
    }
    return NULL;
}
