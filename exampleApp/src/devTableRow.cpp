#include <ctype.h>
#include <stddef.h>
#include <stdlib.h>
#include <string>
#include <vector>

#include "dbAccess.h"
#include "dbLink.h"
#include "dbScan.h"
#include "devSup.h"
#include "recGbl.h"
#include "alarm.h"
#include "errlog.h"
#include "epicsTypes.h"
#include "epicsMath.h"
#include "epicsExport.h"

#include "tableRecord.h"
#include "tableRecordUtil.h"

/*
 * Table Row device support for the table record.
 *
 * Publishes rows of one or more source tables as columns. 
 *
 * Row selection is made by the trailing decimal digits of the column's
 * own name (falling back to the column's position if it has none), e.g.:
 *   field(C00NAME, "row3")   -> publishes row 3
 *   field(C01NAME, "row7")   -> publishes row 7
 * For a row number which does not exist in the source table, the column is
 * filled with zeroes.
 *
 * Each active column must name its own source table record via its CxxINP
 * link, e.g.:
 *   field(C00INP, "TBL:CSV2")
 *   field(C01INP, "TBL:ALT")
 * A column with no CxxINP set publishes a row of zeroes.
 */

struct DevTableRowPvt {
    size_t       maxrows;  /* this record's MAXROWS (output publish cap) */
    std::vector<TableRecordWrapper::DataColumn> data_cols; /* our outputs */
    std::vector<dbCommon *> srcRecs; /* srcRecs[i] = source record */
    std::vector<size_t>     srcRows; /* srcRows[i] = source row */
};

/* Parse the trailing run of decimal digits in `name` (e.g. "row3" -> 3). */ 
static size_t parse_trailing_row_index(const std::string &name, size_t fallback) {
    size_t end = name.size();
    size_t begin = end;
    while (begin > 0 && isdigit((unsigned char)name[begin - 1]))
        --begin;

    if (begin == end)
        return fallback;

    return (size_t)strtoul(name.c_str() + begin, NULL, 10);
}

/* Resolve a source table record by name, given as e.g. "TBL:SRC" */
static dbCommon *resolve_source(dbCommon *pcommon, const std::string &srcname,
                                const char *context) {
    tableRecord *prec = (tableRecord *)pcommon;
    std::string addrname = srcname + ".NUMCOLS";
    DBADDR addr;
    if (dbNameToAddr(addrname.c_str(), &addr) != 0) {
        recGblRecordError(S_db_badField, pcommon,
            "devTableRow: CxxINP names an unknown table record");
        errlogPrintf("%s devTableRow: %s: no such record '%s'\n",
                     prec->name, context, srcname.c_str());
        return NULL;
    }
    return addr.precord;
}

/* RAII record lock using the EPICS dbScanLock/dbScanUnlock API */
struct RowRecLock {
    dbCommon *prec_;
    explicit RowRecLock(dbCommon *p) : prec_(p) { dbScanLock(p); }
    ~RowRecLock() { dbScanUnlock(prec_); }
};

static double cell_to_double(const TableRecordWrapper::CellValue &c) {
    if (c.type == DBF_STRING)
        return epicsNAN;
    if (c.type == DBF_FLOAT || c.type == DBF_DOUBLE)
        return c.fval;
    return (double)c.ival;
}

/* ------------------------------------------------------------------ */
/* Device support                                                     */
/* ------------------------------------------------------------------ */

static long row_init_record(struct dbCommon *pcommon) {
    tableRecord *prec = (tableRecord *)pcommon;

    /* Pass 0: capture each column's raw CxxINP pvname */ 
    if (prec->pact != TABLEREC_DEVINIT_PASS1) {
        TableRecordWrapper rec(pcommon);
        size_t maxcols = rec.max_data_cols();
        std::vector<std::string> *pvnames = new std::vector<std::string>(maxcols);

        for (size_t i = 0; i < maxcols; ++i) {
            DBLINK *inp = &rec.rec.c00inp + i;
            if (inp->type == PV_LINK && inp->value.pv_link.pvname
                        && inp->value.pv_link.pvname[0] != '\0') {
                (*pvnames)[i] = inp->value.pv_link.pvname;
            }
        }

        prec->dpvt = pvnames;
        return TABLEREC_DEVINIT_PASS1;
    }

    /* Pass 1: capture our outputs and resolve each column's own source/row. */
    std::vector<std::string> *pvnames = (std::vector<std::string> *)prec->dpvt;
    DevTableRowPvt *pvt = new DevTableRowPvt();

    TableRecordWrapper rec(pcommon);
    pvt->maxrows = rec.max_data_rows();
    rec.data_cols(pvt->data_cols);

    pvt->srcRecs.reserve(pvt->data_cols.size());
    pvt->srcRows.reserve(pvt->data_cols.size());
    for (size_t i = 0; i < pvt->data_cols.size(); ++i) {
        auto &col = pvt->data_cols[i];
        dbCommon *colSrcRec = NULL;
        const std::string &pvname = (pvnames && i < pvnames->size())
                                     ? (*pvnames)[i] : std::string();

        if (!pvname.empty()) {
            colSrcRec = resolve_source(pcommon, pvname, col.config.name.c_str());
        } else {
            recGblRecordError(S_db_badField, pcommon,
                "devTableRow: column has no CxxINP naming a source table "
                "record; it will publish no rows");
            errlogPrintf("%s devTableRow: column '%s' has no configured source\n",
                         prec->name, col.config.name.c_str());
        }

        pvt->srcRecs.push_back(colSrcRec);
        pvt->srcRows.push_back(parse_trailing_row_index(col.config.name, i));
    }

    delete pvnames;
    rec.set_private(pvt);
    return 0;
}

static long row_read_table(tableRecord *prec) {
    //  Wrap own record & fetch private state
    TableRecordWrapper rec(*prec);
    DevTableRowPvt *pvt = rec.get_private<DevTableRowPvt>();
    long status = 0;

    for (size_t i = 0; i < pvt->data_cols.size(); ++i) {
        auto &out = pvt->data_cols[i];
        if (!*out.val)
            continue;

        dbCommon *srcRec = pvt->srcRecs[i];
        std::vector<TableRecordWrapper::CellValue> cells;
        if (srcRec) {
            // Read the source row under a record lock.
            RowRecLock lk(srcRec);
            TableRecordWrapper src(srcRec);
            src.read_data_row(pvt->srcRows[i], cells);
        }

        epicsUInt32 nOut = (epicsUInt32)cells.size();
        // Cap the output to this record's MAXROWS
        if (nOut > pvt->maxrows)
            nOut = (epicsUInt32)pvt->maxrows;

        epicsFloat64 *dst = (epicsFloat64 *)*out.val;
        // Convert each cell to double and store in the output column
        for (epicsUInt32 j = 0; j < nOut; ++j)
            dst[j] = cell_to_double(cells[j]);

        // Update the output column's metadata
        *out.numrows = nOut;
        *out.chgd    = 1;
    }

    return status;
}

tabledset devTableRow = {
    {5, NULL, NULL, row_init_record, NULL},
    row_read_table
};
epicsExportAddress(dset, devTableRow);
