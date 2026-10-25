# Ben10CosmicDestructionPC

Port nativo experimental para Linux de **Ben 10: Ultimate Alien — Cosmic Destruction**, edição PS2 americana **SLUS-21938 / v1.01**. Base técnica: [GodOfWar2PC](https://github.com/GenissonEmilio/GodOfWar2PC) e PS2Recomp.

**Em desenvolvimento. A compilação e o teste de gameplay são etapas separadas; este repositório não declara a campanha jogável ou o port concluído.** Os resultados reais ficam em [docs/STATUS.md](docs/STATUS.md).

## Como funciona

O executável R5900 do seu disco é analisado e traduzido para C++, depois compilado para Linux x86-64. O runtime fornece gráficos, entrada, arquivos e serviços do PS2. Não há RPCS3, PCSX2 completo ou executável de outro Ben 10 por trás do launcher. Componentes do runtime ainda usam interpretação de IOP/VU quando não há tradução nativa disponível.

O código gerado e os arquivos do jogo permanecem locais. Este repositório não inclui ISO, ELF, BIOS, vídeos, músicas ou outros dados do jogo. Endereços e patches de God of War II não são aplicados ao Ben 10.

## Desenvolvimento

Requisitos: Linux x86-64, Python 3.10+, Docker e driver OpenGL. O build usa Ubuntu 24.04/GCC 13. Na primeira compilação o CMake obtém as dependências abertas declaradas no código.

```bash
tools/linux/build_tools.sh
python3 tools/prepare_game.py '/caminho/Ben 10 Cosmic Destruction.7z'
```

O importador aceita ISO, ZIP, RAR e 7Z, verifica a edição por SHA-256 e recusa links, caminhos fora da pasta, arquivos truncados e compactação excessiva. Para arquivos compactados, instale `libarchive` no sistema. O processo não executa scripts encontrados no arquivo.

A preparação gera `work/disc`, `work/output` e `work/game`; ela não prova que o jogo está funcionando. Os testes de execução devem confirmar menus, gameplay, áudio, controle e persistência antes de uma versão jogável ser publicada.

```bash
python3 -m unittest discover -s tests -v
```

## Licenças e origem

GPL-3.0 para este projeto e o runtime derivado. O importador reutiliza código MIT do launcher de ForbiddenMemoriesPC/GodOfWar2PC; sua licença está em `tools/desktop/LICENSE`. Bibliotecas de terceiros preservam seus avisos. Consulte [docs/PROVENANCE.md](docs/PROVENANCE.md).

Projeto comunitário independente, sem afiliação com os titulares de Ben 10. Cada pessoa fornece sua própria cópia compatível.
