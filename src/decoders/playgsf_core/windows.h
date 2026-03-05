/* Falso windows.h para compilar PlayGSF en Linux/Miyoo */
#ifndef _FAKE_WINDOWS_H
#define _FAKE_WINDOWS_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> /* Para strcasecmp en Linux */

/* Tipos de datos de Microsoft mapeados a tipos estándar */
typedef uint32_t DWORD;
typedef uint16_t WORD;
typedef uint8_t  BYTE;
typedef int      BOOL;
typedef void* HANDLE;
typedef void* HWND;

#define TRUE 1
#define FALSE 0
#define INVALID_HANDLE_VALUE ((HANDLE)-1)
#define MAX_PATH 260

/* Traducción de funciones exclusivas de Windows a POSIX/Linux */
#define stricmp strcasecmp
#define strnicmp strncasecmp

#endif