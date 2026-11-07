/*
 * df_concat.h - stack DataFrames on top of each other (more rows).
 *
 * Example (like pandas pd.concat([a, b])):
 *     const DataFrame *parts[] = { a, b };
 *     df_concat(parts, 2, &out);
 * gives all rows of a, then all rows of b.
 *
 * RULES
 *   - Columns are matched BY NAME, not by position.
 *   - The result has every column that appears in any of the frames, in
 *     the order they first appear (all of the first frame's columns, then
 *     the new ones of the second frame, ...).
 *   - A frame that does not have a column gets missing cells in it.
 *   - Types: a column keeps its type if it is the same everywhere. INT in
 *     one frame and FLOAT in another becomes FLOAT (like pandas). Any
 *     other mix (e.g. INT and STRING) fails with DF_ERR_TYPE_MISMATCH.
 *   - Unlike pandas, an INT column that gets missing cells stays INT:
 *     CPandas marks missing cells with a flag, so it needs no NaN.
 *   - A frame with no columns adds nothing.
 *
 * ERRORS
 *   DF_ERR_INVALID_ARG (nframes == 0), DF_ERR_NULL_ARG (a NULL frame),
 *   DF_ERR_TYPE_MISMATCH (see above).
 *
 * MEMORY OWNERSHIP
 *   - The caller owns *out and frees it with df_free(). It is a deep copy;
 *     the input frames are never changed (the same frame may appear
 *     several times in the list).
 *   - On failure *out is set to NULL and nothing is leaked.
 */
#ifndef DF_CONCAT_H
#define DF_CONCAT_H

#include <stddef.h>
#include "df_error.h"
#include "dataframe.h"

df_status df_concat(const DataFrame *const *frames, size_t nframes,
                    DataFrame **out);

#endif /* DF_CONCAT_H */
