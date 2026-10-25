# Validação

Estado inicial em 25/10/2026:

- Disco PS2 americano SLUS-21938, versão 1.01, identificado e importado: 471 arquivos, uma camada.
- Executável sem símbolos, SHA-256 `f4d62e6475a2b168401f0e21a689a9b37252cf80e054deda1076a74729b01cde`.
- Análise inicial: 14.576 funções heurísticas e 677 correspondências de símbolos do SDK. Esses números não garantem limites de funções corretos.
- Tradução inicial para C++ concluída; destinos indiretos ainda produzem avisos que precisam de validação em execução.
- Tabela de dispatch compactada preservando todos os slots, sem remover entradas de retomada.
- Compilação Linux x86-64 concluída com GCC 13; executável iniciado no computador local, OpenGL 4.6/Intel Mesa.
- Corrigidos acesso a setores/metadados do disco extraído e tratamento de uma leitura pendente.
- Corrigido `DelayThread` no inicializador de um módulo IOP: FMOD passou a concluir sua inicialização.
- Separada a memória interna do runtime do heap usado pelo jogo, com override condicionado ao nome, entry point e CRC do ELF suportado.
- Corrigidos argumentos EE de `sceSifSendCmd` e sua variante interna. Transferências extras agora chegam à RAM do IOP sem sobrescrever o endereço equivalente da RAM EE.
- Implementados registro/remoção e entrega de comandos EE → IOP; o módulo de streaming passou a ler o disco.
- Testes C++ sem ROM validam tarefas durante `DelayThread`, leitura de setores, estado de CD ocupado, argumentos de comandos, reset de handlers e separação dos espaços de memória.
- Os testes limitados do jogo ainda mostram tela preta e tarefas aguardando respostas de streaming. Isso é um bloqueio de runtime, não sucesso de gameplay.
- Menus, combate, áudio, saves e campanha **ainda não validados**.

Nenhuma release jogável será declarada apenas pela conclusão do build.
