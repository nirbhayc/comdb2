#ifndef _DECIMAL_H_
#define _DECIMAL_H_

typedef decQuad   sql_decimal_t;
int sqlitexDecimalToString(sql_decimal_t * dec, char *str, int len);

#endif
