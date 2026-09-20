//
//  encoding.h
//  mkxp-z
//
//  Created by ゾロアーク on 6/22/21.
//

#ifndef encoding_h
#define encoding_h

#include <string>
#include <string.h>

#include "util/encoding.h"
#include <iconv.h>
#include <uchardet.h>
#include "prism-trace.h"
#include <errno.h>

namespace Encoding {

static std::string getCharset(std::string &str) {
    uchardet_t ud = uchardet_new();
    uchardet_handle_data(ud, str.c_str(), str.length());
    uchardet_data_end(ud);
    
    const char *guess = uchardet_get_charset(ud);
    prismTrace((std::string("UCHARDET: adivinhou '") + (guess ? guess : "(nulo)") +
                "' para \"" + str + "\"").c_str());

    /* uchardet_get_charset pode devolver nulo, e construir std::string a
       partir de nulo e comportamento indefinido. */
    std::string ret(guess ? guess : "");
    uchardet_delete(ud);
    
    if (ret.empty())
        throw Exception(Exception::MKXPError, "Could not detect string encoding", str.c_str());
    return ret;
}

static std::string convertString(std::string &str, const char *charset) {
    // Conversion doesn't need to happen if it's already UTF-8
    if (!strcmp(charset, "UTF-8") || !strcmp(charset, "ASCII")) {
        return std::string(str);
    }
    
    prismTrace((std::string("ICONV: abrindo conversao de '") + charset +
                "' para UTF-8").c_str());

    iconv_t cd = iconv_open("UTF-8", charset);

    /*
     * iconv_open devolve (iconv_t)-1 quando nao conhece a codificacao, e o
     * codigo original passava esse -1 direto para iconv(), que o trata como
     * ponteiro e o dereferencia. Isso derruba o processo com violacao de
     * acesso dentro da libiconv, sem mensagem nenhuma, no meio da leitura do
     * Game.ini. Aqui vira excecao, que readGameINI ja captura para cair no
     * titulo padrao.
     */
    if (cd == (iconv_t)-1) {
        prismTrace("ICONV: iconv_open FALHOU, codificacao desconhecida");
        throw Exception(Exception::MKXPError,
                        "Unknown encoding (Guessed: %s)", charset);
    }

    size_t inLen = str.size();
    size_t outLen = inLen * 4;
    std::string buf(outLen, '\0');
    char *inPtr = const_cast<char*>(str.c_str());
    char *outPtr = const_cast<char*>(buf.c_str());
    
    errno = 0;
    size_t result = iconv(cd, &inPtr, &inLen, &outPtr, &outLen);
    
    iconv_close(cd);
    
    if (result != (size_t)-1 && errno == 0)
    {
        buf.resize(buf.size()-outLen);
    }
    else {
        throw Exception(Exception::MKXPError, "Unable to convert string (Guessed encoding: %s)", charset);
    }
    
    return buf;
}

static std::string convertString(std::string &str) {
    return convertString(str, getCharset(str).c_str());
}
}

#endif /* encoding_h */
