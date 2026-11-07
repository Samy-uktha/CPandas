/*
 * df_date.c - calendar dates <-> days since 1970-01-01.
 *
 * The two conversions use the well-known "days from civil" method: shift
 * the year so that it starts on 1 March (then the leap day is the last day
 * of the year), count whole 400-year "eras" (each exactly 146097 days),
 * and the rest is plain arithmetic. No loops over years or months.
 */
#include "df_date.h"

#include <stdio.h>

static int is_leap_year(int year)
{
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

static int days_in_month(int year, int month)
{
    static const int lengths[12] = {31, 28, 31, 30, 31, 30,
                                    31, 31, 30, 31, 30, 31};
    if (month == 2 && is_leap_year(year)) {
        return 29;
    }
    return lengths[month - 1];
}

df_status df_date_from_ymd(int year, int month, int day, int64_t *days)
{
    if (days == NULL) {
        return DF_ERR_NULL_ARG;
    }
    if (year < 1 || year > 9999 || month < 1 || month > 12 || day < 1 ||
        day > days_in_month(year, month)) {
        return DF_ERR_INVALID_ARG;
    }

    int64_t y = year;
    if (month <= 2) {
        y = y - 1;              /* January and February belong to the
                                   previous "March year" */
    }
    int64_t era = y / 400;      /* y >= 0 here, so no rounding issue */
    int64_t year_of_era = y - era * 400;                       /* 0..399 */
    int64_t m = (month > 2) ? month - 3 : month + 9;           /* Mar = 0 */
    int64_t day_of_year = (153 * m + 2) / 5 + day - 1;         /* 0..365 */
    int64_t day_of_era = year_of_era * 365 + year_of_era / 4 -
                         year_of_era / 100 + day_of_year;      /* 0..146096 */
    *days = era * 146097 + day_of_era - 719468;    /* 719468 = 1970-01-01 */
    return DF_OK;
}

void df_date_to_ymd(int64_t days, int *year, int *month, int *day)
{
    int64_t z = days + 719468;
    /* era = floor(z / 146097), also for negative z */
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t day_of_era = z - era * 146097;                     /* 0..146096 */
    int64_t year_of_era = (day_of_era - day_of_era / 1460 +
                           day_of_era / 36524 - day_of_era / 146096) / 365;
    int64_t y = year_of_era + era * 400;
    int64_t day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 -
                                        year_of_era / 100);
    int64_t mp = (5 * day_of_year + 2) / 153;                  /* Mar = 0 */
    int64_t d = day_of_year - (153 * mp + 2) / 5 + 1;
    int64_t m = (mp < 10) ? mp + 3 : mp - 9;
    if (m <= 2) {
        y = y + 1;
    }

    if (year != NULL) {
        *year = (int)y;
    }
    if (month != NULL) {
        *month = (int)m;
    }
    if (day != NULL) {
        *day = (int)d;
    }
}

/* Reads `count` digits starting at text into *value. 0 if one is not a
   digit. */
static int read_digits(const char *text, int count, int *value)
{
    int v = 0;
    for (int k = 0; k < count; k++) {
        if (text[k] < '0' || text[k] > '9') {
            return 0;
        }
        v = v * 10 + (text[k] - '0');
    }
    *value = v;
    return 1;
}

df_status df_date_parse(const char *text, int64_t *days)
{
    if (text == NULL || days == NULL) {
        return DF_ERR_NULL_ARG;
    }
    int year = 0;
    int month = 0;
    int day = 0;
    /* YYYY-MM-DD: positions 0-3, '-' at 4, 5-6, '-' at 7, 8-9, end at 10 */
    if (!read_digits(text, 4, &year) || text[4] != '-' ||
        !read_digits(text + 5, 2, &month) || text[7] != '-' ||
        !read_digits(text + 8, 2, &day) || text[10] != '\0') {
        return DF_ERR_PARSE;
    }
    if (df_date_from_ymd(year, month, day, days) != DF_OK) {
        return DF_ERR_PARSE;    /* looks like a date but does not exist */
    }
    return DF_OK;
}

void df_date_format(int64_t days, char *buf, size_t size)
{
    if (buf == NULL || size == 0) {
        return;
    }
    int year = 0;
    int month = 0;
    int day = 0;
    df_date_to_ymd(days, &year, &month, &day);
    snprintf(buf, size, "%04d-%02d-%02d", year, month, day);
}
