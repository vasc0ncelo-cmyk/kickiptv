#pragma once
// Chave da API do TMDB (gratuita, v3) usada so para buscar a LOGO dos filmes/series no carrossel do Inicio.
// Sem chave o app funciona igual: o titulo aparece em texto.
//  1) Segredo TMDB_KEY do GitHub (o workflow gera src/tmdbkey_secret.hpp), ou
//  2) escreva a chave entre aspas abaixo.
#if defined(__has_include)
#if __has_include("tmdbkey_secret.hpp")
#include "tmdbkey_secret.hpp"      // define: static const char *kSecretTmdbKey = "...";
#define KICKIPTV_HAS_SECRET_TMDB 1
#endif
#endif
inline const char *tmdbKey() {
#ifdef KICKIPTV_HAS_SECRET_TMDB
    return kSecretTmdbKey;
#else
    return "15fa049ad9d5387ad509bb123f4bcc65";
#endif
}
