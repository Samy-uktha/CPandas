/*
 * df_error.h - status codes shared by every module of the library.
 *
 * Convention: any function that can fail returns a df_status. Results are
 * handed back through out-parameters. On failure the out-parameter is set to
 * NULL (or left untouched for non-pointer outputs) and every input object is
 * left exactly as it was before the call.
 *
 * Owner: Person A
 */
#ifndef DF_ERROR_H
#define DF_ERROR_H

typedef enum {
    DF_OK = 0,
    DF_ERR_NULL_ARG,          /* a required pointer argument was NULL            */
    DF_ERR_INVALID_ARG,       /* argument value not allowed (e.g. empty name)    */
    DF_ERR_NOMEM,             /* malloc/realloc/strdup failed                    */
    DF_ERR_TYPE_MISMATCH,     /* e.g. appending a string to an int Series        */
    DF_ERR_INDEX_OUT_OF_RANGE,/* row or column index >= length                   */
    DF_ERR_NOT_FOUND,         /* no column with that name                        */
    DF_ERR_DUPLICATE_NAME,    /* a column with that name already exists          */
    DF_ERR_LENGTH_MISMATCH,   /* column length differs from the frame's row count*/
    DF_ERR_MISSING_VALUE,     /* tried to read a value from a missing cell       */
    DF_ERR_IO,                /* fopen/fread/fwrite failed                       */
    DF_ERR_PARSE              /* malformed CSV input                             */
} df_status;

/* Returns a static, human-readable string for a status code.
 * Never returns NULL; unknown codes yield "unknown error". Do not free. */
const char *df_strerror(df_status s);

#endif /* DF_ERROR_H */