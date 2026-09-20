#include "prism-trace.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#else
#include <stdio.h>
#endif

static const char *tracePath() {
    return getenv("MKXPZ_LOG_FILE");
}

void prismTrace(const char *message) {
    const char *path = tracePath();
    if (!path || !*path || !message)
        return;

#ifdef _WIN32
    /* FILE_APPEND_DATA mais OPEN_ALWAYS: cria na primeira vez e acrescenta nas
       seguintes. O arquivo fecha a cada linha, entao o conteudo ja esta em
       disco quando o processo cai. */
    HANDLE file = CreateFileA(path, FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return;

    DWORD written = 0;
    WriteFile(file, message, (DWORD)strlen(message), &written, NULL);
    WriteFile(file, "\r\n", 2, &written, NULL);
    CloseHandle(file);
#else
    FILE *file = fopen(path, "a");
    if (!file)
        return;

    fputs(message, file);
    fputc('\n', file);
    fclose(file);
#endif
}

#ifdef _WIN32
/*
** O gravador de minidump.
**
** Com ele, um 0xC0000005 deixa de ser um numero e vira instrucao, registradores
** e pilha de chamadas, que e o que o WinDbg precisa. O arquivo sai ao lado do
** rastro, com a extensao trocada para .dmp.
*/
static LONG WINAPI prismCrashHandler(EXCEPTION_POINTERS *ep) {
    prismTrace("!!! EXCECAO NAO TRATADA, gravando minidump");

    const char *path = tracePath();
    if (!path || !*path)
        return EXCEPTION_CONTINUE_SEARCH;

    char dumpPath[1024];
    strncpy(dumpPath, path, sizeof(dumpPath) - 5);
    dumpPath[sizeof(dumpPath) - 5] = '\0';
    strcat(dumpPath, ".dmp");

    HANDLE file = CreateFileA(dumpPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return EXCEPTION_CONTINUE_SEARCH;

    MINIDUMP_EXCEPTION_INFORMATION info;
    memset(&info, 0, sizeof(info));
    info.ThreadId = GetCurrentThreadId();
    info.ExceptionPointers = ep;
    info.ClientPointers = FALSE;

    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                      MiniDumpWithFullMemory, &info, NULL, NULL);
    CloseHandle(file);

    prismTrace("minidump gravado");
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

void prismInstallCrashHandler() {
#ifdef _WIN32
    SetUnhandledExceptionFilter(prismCrashHandler);
#endif
}
