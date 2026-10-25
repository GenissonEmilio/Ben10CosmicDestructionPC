# Validação

Estado inicial em 25/10/2026:

- Disco PS2 americano SLUS-21938, versão 1.01, identificado e importado: 471 arquivos, uma camada.
- Executável sem símbolos, SHA-256 `f4d62e6475a2b168401f0e21a689a9b37252cf80e054deda1076a74729b01cde`.
- Análise inicial: 14.576 funções heurísticas e 677 correspondências de símbolos do SDK. Esses números não garantem limites de funções corretos.
- Tradução inicial para C++ concluída; destinos indiretos ainda produzem avisos que precisam de validação em execução.
- Tabela de dispatch compactada preservando todos os slots, sem remover entradas de retomada.
- Compilação Linux e teste do jogo em andamento.
- Menus, combate, áudio, saves e campanha **ainda não validados**.

Nenhuma release jogável será declarada apenas pela conclusão do build.
