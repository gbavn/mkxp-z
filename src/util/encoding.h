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

/*
 * Um texto ja em UTF-8 nao precisa de adivinhacao nenhuma.
 *
 * O uchardet erra, e erra feio em arquivo curto ou cheio de pontuacao: aqui
 * ele olhou o mkxp.json, que e UTF-8 com comentarios e a palavra "Pokemon"
 * acentuada, e concluiu MAC-CENTRALEUROPE. Como a conversao a partir de um
 * palpite errado estraga o texto mesmo quando funciona, o caminho seguro e
 * reconhecer UTF-8 valido e devolver como esta.
 */
static bool isValidUTF8(const std::string &str) {
    const unsigned char *p = (const unsigned char *)str.c_str();
    size_t left = str.size();

    while (left > 0) {
        unsigned char c = *p;
        size_t extra;

        if (c < 0x80)                       extra = 0;
        else if ((c & 0xE0) == 0xC0)        extra = 1;
        else if ((c & 0xF0) == 0xE0)        extra = 2;
        else if ((c & 0xF8) == 0xF0)        extra = 3;
        else                                return false;

        if (extra >= left)
            return false;

        for (size_t i = 1; i <= extra; ++i)
            if ((p[i] & 0xC0) != 0x80)
                return false;

        p += extra + 1;
        left -= extra + 1;
    }

    return true;
}

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
        /*
         * O uchardet e o libiconv nem sempre falam a mesma lingua: o primeiro
         * devolveu 'MAC-CENTRALEUROPE' e o segundo so conhece esse mesmo
         * conjunto escrito de outro jeito. Antes de desistir, tenta as formas
         * mais comuns do mesmo nome.
         */
        std::string semTraco;
        for (const char *c = charset; *c; ++c)
            if (*c != '-' && *c != '_')
                semTraco += *c;

        cd = iconv_open("UTF-8", semTraco.c_str());

        if (cd == (iconv_t)-1) {
            /*
             * Desistir aqui nao pode custar o arquivo inteiro. Quem chama isto
             * inclui a leitura do mkxp.json, e la a excecao era capturada e
             * trocada por "segue com os valores padrao", o que faz toda a
             * configuracao do jogo sumir sem aviso. Devolver o texto como veio
             * e pior que converter certo e melhor que perder tudo.
             */
            prismTrace("ICONV: iconv_open FALHOU nas duas formas, devolvendo o texto como esta");
            return std::string(str);
        }

        prismTrace("ICONV: funcionou sem os tracos no nome");
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
    if (isValidUTF8(str))
        return std::string(str);

    return convertString(str, getCharset(str).c_str());
}
}

#endif /* encoding_h */
