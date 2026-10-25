/*
 * cpandas.h - include this one header to get the whole library.
 *
 *   Data layer  (Person A): series.h, dataframe.h, csv.h, df_error.h
 *   Query layer (Person B): df_rows.h, df_filter.h, df_sort.h, df_stats.h,
 *                           df_groupby.h, df_join.h
 */
#ifndef CPANDAS_H
#define CPANDAS_H

#include "df_error.h"
#include "series.h"
#include "dataframe.h"
#include "csv.h"

#include "df_rows.h"
#include "df_filter.h"
#include "df_sort.h"
#include "df_stats.h"
#include "df_groupby.h"
#include "df_join.h"

#endif /* CPANDAS_H */
