/*
** prism-trace.h
**
** Rastro de inicializacao, para diagnosticar crash que acontece cedo demais
** para qualquer log normal.
**
** Escreve direto no arquivo, com a chamada do sistema, sem passar por
** iostream, por stdio, por buffer do CRT nem pelo SDL. Isso nao e preciosismo:
** tentamos antes com redirecionamento do cmd e com freopen em stderr, e as
** duas vezes o arquivo saiu vazio, sem sabermos se era o programa que morria
** ou o mecanismo de captura que falhava. Aqui nao ha essa duvida.
**
** O caminho vem da variavel de ambiente MKXPZ_LOG_FILE. Sem ela, tudo isto e
** silencioso e nao custa nada.
*/

#ifndef PRISM_TRACE_H
#define PRISM_TRACE_H

/** Acrescenta uma linha ao rastro. Abre e fecha o arquivo a cada chamada, de
 *  proposito: assim nada se perde quando o processo morre de repente. */
void prismTrace(const char *message);

/** Instala o gravador de minidump, para o crash virar pilha de chamadas. */
void prismInstallCrashHandler();

#endif // PRISM_TRACE_H
