#pragma once
// Chaves da API OFICIAL da Kick (para a tela "Lives agora"). Crie um app gratis em kick.com > Configuracoes > Desenvolvedor.
//  1) Segredos KICK_CLIENT_ID e KICK_CLIENT_SECRET do GitHub (o workflow gera src/kickkey_secret.hpp), ou
//  2) escreva as chaves entre aspas abaixo (so use com repositorio PRIVADO).
// Sem chaves o app funciona igual: so a lista "Lives agora" fica indisponivel (favoritos, historico e @streamer seguem normais).
#if defined(__has_include)
#if __has_include("kickkey_secret.hpp")
#include "kickkey_secret.hpp"      // define: kSecretKickId e kSecretKickSecret (const char *)
#define KICKIPTV_HAS_SECRET_KICK 1
#endif
#endif
inline const char *kickClientId() {
#ifdef KICKIPTV_HAS_SECRET_KICK
    return kSecretKickId;
#else
    return "01M7C4ZZ9TFVKNVX3Y14FN5FY3";
#endif
}
inline const char *kickClientSecret() {
#ifdef KICKIPTV_HAS_SECRET_KICK
    return kSecretKickSecret;
#else
    return "7d844bdba109832e5c75d3b0bfd028c60b3796a22fa4d748b5236e89ca2cad5c";
#endif
}
