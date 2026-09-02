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
 * Publishes rows of a source table as columns.
 *
 * The INP field is an instrument-IO link naming the source table record:
 *   field(INP, "@TBL:SRC")
 *
 * Every active output column must be DBF_DOUBLE.
 *
 * Row selection:
 *   By default, output column i publishes source row i (its own position).
 *   To select a specific, arbitrary source row instead, give the column a
 *   name ending in that row's decimal number, e.g.:
 *     field(C00NAME, "row3")   -> publishes TBL:SRC row 3
 *     field(C01NAME, "row7")   -> publishes TBL:SRC row 7
 *     field(C02NAME, "row10")  -> publishes TBL:SRC row 10
 */

struct DevTableRowPvt {
    dbCommon    *srcRec;   /* the source table record */
    size_t       maxrows;  /* this record's MAXROWS (== number of output columns used as row index cap) */
    std::vector<TableRecordWrapper::DataColumn> data_cols; /* our outputs, one per row of the source */
    std::vector<size_t> srcRows; /* srcRows[i] = source row published by data_cols[i] */
};

/* Parse the trailing run of decimal digits in `name` (e.g. "row3" -> 3).
 * If `name` has no trailing digits, returns `fallback` (the column's own
 * positional index) so unselected columns keep the original 1:1 mapping. */
static size_t parse_trailing_row_index(const std::string &name, size_t fallback) {
    size_t end = name.size();
    size_t begin = end;
    while (begin > 0 && isdigit((unsigned char)name[begin - 1]))
        --begin;

    if (begin == end)
        return fallback;

    return (size_t)strtoul(name.c_str() + begin, NULL, 10);
}

/* Simple RAII record lock using the EPICS dbScanLock/dbScanUnlock API */
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

static long row_init_record(struct dbCommon *pcommon) {
    tableRecord *prec = (tableRecord *)pcommon;
    struct link *plnk = &prec->inp;

    /* Pass 0: validate INP, then defer to pass 1 (active columns are only
       known after the record validates column names, post pass-0). */
    if (prec->pact != TABLEREC_DEVINIT_PASS1) {
        if (plnk->type != INST_IO || !plnk->value.instio.string
                                  || plnk->value.instio.string[0] == '\0') {
            recGblRecordError(S_db_badField, pcommon,
                "devTableRow: INP must be an instrument-IO link naming the "
                "source table record, e.g. field(INP, \"@TBL:SRC\")");
            return S_db_badField;
        }
        return TABLEREC_DEVINIT_PASS1;
    }

    /* Pass 1: resolve the source record and capture our outputs. */
    std::string srcname(plnk->value.instio.string);
    std::string addrname = srcname + ".NUMCOLS";
    DBADDR addr;
    if (dbNameToAddr(addrname.c_str(), &addr) != 0) {
        recGblRecordError(S_db_badField, pcommon,
            "devTableRow: INP names an unknown table record");
        errlogPrintf("%s devTableRow: no such record '%s'\n", prec->name, srcname.c_str());
        return S_db_badField;
    }

    DevTableRowPvt *pvt = new DevTableRowPvt();
    pvt->srcRec = addr.precord;

    TableRecordWrapper rec(pcommon);
    pvt->maxrows = rec.max_data_rows();
    rec.data_cols(pvt->data_cols);

    pvt->srcRows.reserve(pvt->data_cols.size());
    for (size_t i = 0; i < pvt->data_cols.size(); ++i)
        pvt->srcRows.push_back(
            parse_trailing_row_index(pvt->data_cols[i].config.name, i));

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

        std::vector<TableRecordWrapper::CellValue> cells;
        {
            // Read the source row under a record lock.
            RowRecLock lk(pvt->srcRec);
            TableRecordWrapper src(pvt->srcRec);
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
