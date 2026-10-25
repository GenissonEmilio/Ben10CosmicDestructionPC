# Retomada após reinício — 25/10/2026

O usuário pediu o envio do código ao GitHub para reiniciar o notebook. O port continua em desenvolvimento. Não reiniciar nem desligar o computador por conta própria.

Repositório: https://github.com/GenissonEmilio/Ben10CosmicDestructionPC

Branch: `codex/ps2-native-port`.

Pasta local: `/home/genissonemilio/Documentos/ChatGPT/descompilador/Ben10CosmicDestructionPC`.

## Estado comprovado

O executável nativo Linux compilado com `-O1` reproduz vídeos, permite confirmar o nome do perfil e iniciar uma campanha. O teste 27 chegou à primeira cena 3D. O combate e o desempenho ainda não foram validados; há corrupção visual de texturas e um bloqueio no motor de scripts.

A correção do tradutor no commit `d2aeb76` preserva a instrução real de delay slot quando a análise termina uma função no salto. Isso corrigiu a passagem incorreta de um ponteiro de pilha para `0x254780`, que antes sobrescrevia um objeto da interface ao confirmar o nome. Não substituir essa correção por retornos artificiais ou chamadas ignoradas.

Os fluxos de CI anteriores `40210658867` e `40209459357` passaram. O primeiro inclui tradução, compilação e execução de quatro programas sintéticos com JAL, BEQ, BEQL e BNEL. Esses testes não contêm arquivos de jogo.

## Bloqueio a investigar

No teste 27, após os diálogos iniciais da fase:

```text
IndirectCall/JALR source=0x002BD004 target=0x00DB8088
ra=0x002BD00C sp=0x005F13F0 gp=0x005803F0
a0=0x24 a1=0x10600058 a2=0x24 a3=0x10600058
```

O destino é uma tabela de dados no heap, não código MIPS. A função `0x2BCF30` obtém o descritor de `a0 + 12` e o destino de `descritor + 16`. O override diagnóstico agora registra descritor, destino, RA e oito palavras quando o destino não está registrado. Só é ativado com `COSMIC_DIAGNOSTICS`; preserva a chamada original.

O teste 28 contém esse diagnóstico, mas foi encerrado pelo usuário antes de concluir a reprodução. Seu `result.json` registra `exit_code: -15`, consequência do SIGTERM solicitado para liberar o notebook, e não conclusão de gameplay.

Também investigar as texturas com faixas/blocos da primeira fase. Não há prova de que o perfil nomeado persista em disco: os arquivos iniciais de save existem e foram relidos, mas o hash de `profile1.dat` não mudou no teste 27.

## Arquivos locais preservados, fora do Git

- `work/disc.iso`, `work/disc/SLUS_219.38` e os dados importados permanecem locais. Edição PS2 americana SLUS-21938 v1.01; SHA-256 do ELF: `f4d62e6475a2b168401f0e21a689a9b37252cf80e054deda1076a74729b01cde`.
- `work/game`: executável de investigação O1, com o diagnóstico de scripts compilado. `work/build.json` registra sua identidade; não é uma release.
- `work/objects-O1`, `work/output`, `work/generated-extra`, `work/output-baseline` e `work/output-before-delay`: preservar o cache e as traduções para comparação.
- `work/compiler`: kit portátil anterior, sem `libbase.a` de God of War II. O tradutor foi atualizado com a correção de delay slots; várias unidades do runtime são recompiladas pelo projeto antes de vincular as bibliotecas genéricas.
- `work/bin/ps2_recomp-delay`, `work/bin/disasm`: ferramentas locais de investigação.
- **`artifacts/compiler`: kit próprio completo, recém-compilado**, ID `eac4df0c3dadc0a28016a14b`. O build terminou antes do encerramento, registrado em `artifacts/build-own-kit.log`. Ainda precisa de regressões e execução real com esse kit.
- `artifacts/probe-27/game.log`, `ram-blocked.bin` e `frames/`: reprodução anterior. `frame_140.png` mostra o menu, `frame_220.png` a introdução, `frame_280.png` a preparação e `frame_360.png` a cena 3D.
- `artifacts/probe-28/`: logs, saves privados e resultado da execução interrompida.
- `artifacts/probe-25/watch.log` e `artifacts/probe-26/watch.log`: investigação da corrupção de memória já corrigida pelo delay slot.

Não enviar ISO, ELF, saves, imagens do jogo, bibliotecas com dados do jogo ou código traduzido ao GitHub. Não limpar essas pastas durante a retomada.

## Próxima sessão

1. Consultar `docs/STATUS.md` e `git status`. Não repetir a importação nem uma compilação completa sem necessidade.
2. Validar o kit próprio com os testes nativos e de tradução, seguindo `.github/workflows/linux.yml`; o host tem GCC C, mas não `g++`/CMake, disponíveis na imagem Docker `cosmic-ps2-linux-builder`.
3. Retomar a investigação do descritor inválido, com execuções limitadas pelo probe e `COSMIC_DIAGNOSTICS=1`, preservando os saves privados de teste. Não mascarar destinos inválidos com stubs.
4. Corrigir a renderização e validar movimento, combate, áudio e saves antes de medir FPS ou declarar uma versão jogável.
5. Antes do build final O2, incluir a identidade do compilador nas chaves dos objetos. O cache atual usa fontes, headers e flags; a preparação já considera o manifesto e o hash do tradutor. O link agora é atômico, mas ainda precisa ser exercitado e o marcador `build.json` ainda não é gravado atomicamente.
6. Finalizar instalação, empacotamento e release do launcher apenas depois da validação. A interface GTK existe em `tools/desktop/main.py`; ainda não há instalador ou release deste port.

Limitar builds a um ou dois jobs. No notebook de 16 GB, usar escopos `systemd-run --user --scope` com `CPUQuota=200%`, `MemoryMax=2200M` e `MemorySwapMax=0` para o build nativo; probes com `MemoryMax=1800M`. Docker requer `--security-opt label=disable` para o bind mount no Fedora, sem relabelar pastas do usuário.

O projeto base `../GodOfWar2PC` possui alterações preexistentes em `README.md` e `PS2Recomp/ps2xRuntime/src/lib/ps2_pad.cpp`; não sobrescrevê-las. Não mexer nos outros projetos ou serviços Docker do usuário.
