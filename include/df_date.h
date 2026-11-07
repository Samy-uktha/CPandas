/*
 * df_date.h - convert calendar dates to and from the number stored in a
 * DTYPE_DATE Series.
 *
 * A date is stored as the number of days since 1970-01-01 (that day is 0,
 * 1970-01-02 is 1, 1969-12-31 is -1). Storing one number makes dates easy
 * to compare, sort and subtract, just like integers.
 *
 * Text form: always "YYYY-MM-DD" (ISO 8601), e.g. "2024-02-29". This is
 * what the CSV reader recognises and what the writer and df_print() show.
 *
 * Example:
 *     int64_t days;
 *     if (df_date_parse("2024-03-15", &days) == DF_OK) {
 *         series_append_date(col, days);
 *     }
 */
#ifndef DF_DATE_H
#define DF_DATE_H

#include <stddef.h>
#include <stdint.h>
#include "df_error.h"

/* Longest text df_date_format() writes, plus the '\0'. */
#define DF_DATE_TEXT_SIZE 16

/* Days since 1970-01-01 for a calendar date (Gregorian calendar).
 * Fails with DF_ERR_INVALID_ARG for an impossible date such as month 13 or
 * 2023-02-29, or a year outside 1..9999. *days is not changed then. */
df_status df_date_from_ymd(int year, int month, int day, int64_t *days);

/* The calendar date of a day number. Any pointer may be NULL if that part
 * is not needed. */
void df_date_to_ymd(int64_t days, int *year, int *month, int *day);

/* Reads exactly "YYYY-MM-DD" (4 digits, '-', 2 digits, '-', 2 digits, and
 * nothing else) and checks that the date exists. DF_ERR_PARSE otherwise. */
df_status df_date_parse(const char *text, int64_t *days);

/* Writes the date as "YYYY-MM-DD" into buf (size should be at least
 * DF_DATE_TEXT_SIZE; the text is cut short to fit a smaller buf). */
void df_date_format(int64_t days, char *buf, size_t size);

#endif /* DF_DATE_H */
