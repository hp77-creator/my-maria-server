#ifndef FRM_PARSER_H
#define FRM_PARSER_H

#include "table.h"
#include "sql_class.h"
bool parse_frm_binary_standalone(
    THD *thd,
    const uchar *frm_image,
    size_t frm_length,
    TABLE_SHARE *share,
    bool validate_only = false
);

#endif