# Origem do código

O diretório `PS2Recomp` e os geradores `tools/iop` / `tools/vu1` foram trazidos dos arquivos versionados da base local GodOfWar2PC. A alteração local de entrada por controle dessa base foi preservada na cópia; o projeto original não foi alterado.

- https://github.com/GenissonEmilio/GodOfWar2PC
- https://github.com/cxldsakeb/GoW2-nativo-PS2
- https://github.com/ran-j/PS2Recomp

O runtime inclui contribuições e referências de Waffle565, YYOzcan/Tobiichi-Port e PCSX2, com avisos preservados nos arquivos. O backend de apresentação OpenGL veio da base Linux existente.

Os overrides de God of War II, seu mapa de funções, suas capturas de IOP/VU e seus endereços de patch não foram importados como configuração de Ben 10. O executável de entrada e o título do runtime foram adaptados para o novo projeto.

Para a primeira prova de execução local, foram reutilizadas bibliotecas estáticas genéricas e o GCC portátil do kit de GodOfWar2PC; `libbase.a`, que contém patches específicos de God of War II, foi excluída. O script de build público recompila as bibliotecas a partir do código incluído.

O projeto https://github.com/ExtremestoneGG/Cosmic-Recompi foi consultado; ele tem como alvo Xbox 360/Windows e não foi incorporado a esta implementação PS2.

O helper de testes sintéticos `tests/support/iop_compat_test_support.h` também vem da base PS2Recomp/GodOfWar2PC (GPL-3.0). Os testes não contêm código ou arquivos de jogos.
