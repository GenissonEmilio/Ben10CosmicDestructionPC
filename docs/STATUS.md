# Validação

Estado de desenvolvimento em 25/10/2026:

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
- Implementadas as variantes IOP `SifSendCmdIntr`/`iSifSendCmdIntr`, incluindo callback de conclusão da transferência.
- Ativada a ponte nativa de funções LIBSD somente para o módulo cujo tamanho e hash de código foram verificados; FMOD deixa de aguardar a inicialização do áudio.
- A busca de arquivo com camada usa o índice real do disco de camada única suportado.
- Acrescentados destinos indiretos de tabelas/vtables e continuações de chamadas ausentes no mapa inicial.
- Testes C++ sem ROM validam tarefas durante `DelayThread`, leitura de setores, estado de CD ocupado, argumentos de comandos, callbacks de conclusão, reset de handlers e separação dos espaços de memória.
- Onze testes Python validam importação, preservação do dispatch, rollback da atualização do código gerado e invalidação do cache quando o kit muda.
- Em testes locais limitados, o executável nativo exibiu o carregamento com Omnitrix, criou os arquivos de save e voltou a lê-los em outra execução, reproduziu vídeos de abertura e chegou à tela original "Press Start". Isso ainda não valida uma partida.
- Corrigidos preservação dos callbacks durante `sceMpegInit` e layout do workspace MPEG, com testes sintéticos de entrega de pacotes e frames sem sobrescrever memória do executável.
- Corrigido o tradutor para ler a instrução de delay slot real quando o mapa corta uma função imediatamente após um salto. Quatro programas ELF sintéticos são traduzidos, compilados e executados para verificar JAL, BEQ, BEQL e BNEL, incluindo a anulação do slot. O CI executa esses testes sem arquivos de jogo.
- Depois da correção de delay slots, o teste local confirmou a aceitação do nome do perfil, o menu "New Game", a introdução da campanha, a preparação da missão e a renderização da primeira cena 3D, com o diálogo inicial de Gwen.
- A cena 3D apresenta texturas com faixas/blocos. Depois dos diálogos iniciais, uma chamada indireta do motor de scripts em `0x002BD004` tenta executar `0x00DB8088`, que contém dados. A causa ainda está em investigação; ignorar essa chamada não seria uma correção.
- Acrescentado diagnóstico opcional do descritor de scripts, ativado por `COSMIC_DIAGNOSTICS`, para investigar esse bloqueio.
- O kit aberto Linux foi compilado integralmente a partir deste código em `artifacts/compiler` (ID local `eac4df0c3dadc0a28016a14b`). Falta validar o jogo usando esse kit recém-gerado; o executável de investigação usa o kit anterior com unidades do runtime atualizadas.
- O link de novas versões passa a usar um arquivo temporário e substituição atômica após sucesso, preservando o executável anterior se o linker falhar. Esta alteração recebeu verificação de sintaxe; ainda falta exercitar o caminho em um novo link nativo.
- Movimento, combate, áudio correto, persistência do perfil nomeado e campanha **ainda não validados**. Não há medição confiável de FPS nem release jogável.

Ponto de retomada e artefatos locais: [HANDOFF.md](HANDOFF.md). Os testes em execução foram encerrados a pedido do usuário para permitir o reinício do notebook.

Nenhuma release jogável será declarada apenas pela conclusão do build.
