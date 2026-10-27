/*-------------------------------------------------------------------------
 *
 * prolly_function_scan.h
 *	  What a scan standing in for a dolt function's must do as the
 *	  function would.
 *
 * src/include/access/prolly_function_scan.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROLLY_FUNCTION_SCAN_H
#define PROLLY_FUNCTION_SCAN_H

/*
 * The EXECUTE check (and execute hook) a function scan makes of funcid as
 * it begins, for a scan that reads what the function would without calling
 * it: one who can't execute the function can't read through the scan.
 */
extern void prolly_function_scan_check(Oid funcid);

#endif							/* PROLLY_FUNCTION_SCAN_H */
