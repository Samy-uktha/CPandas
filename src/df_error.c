#include "df_error.h"

const char *df_strerror(df_status s)
{
    switch (s) {
    case DF_OK:                    return "success";
    case DF_ERR_NULL_ARG:          return "required argument was NULL";
    case DF_ERR_INVALID_ARG:       return "invalid argument value";
    case DF_ERR_NOMEM:             return "out of memory";
    case DF_ERR_TYPE_MISMATCH:     return "column type mismatch";
    case DF_ERR_INDEX_OUT_OF_RANGE:return "index out of range";
    case DF_ERR_NOT_FOUND:         return "column not found";
    case DF_ERR_DUPLICATE_NAME:    return "duplicate column name";
    case DF_ERR_LENGTH_MISMATCH:   return "column length does not match frame";
    case DF_ERR_MISSING_VALUE:     return "cell is missing";
    case DF_ERR_IO:                return "I/O error";
    case DF_ERR_PARSE:             return "parse error";
    }
    return "unknown error";
}