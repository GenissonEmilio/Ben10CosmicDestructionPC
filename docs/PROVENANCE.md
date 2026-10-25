# Origem do código

O diretório `PS2Recomp` e os geradores `tools/iop` / `tools/vu1` foram trazidos dos arquivos versionados da base local GodOfWar2PC. A alteração local de entrada por controle dessa base foi preservada na cópia; o projeto original não foi alterado.

- https://github.com/GenissonEmilio/GodOfWar2PC
- https://github.com/cxldsakeb/GoW2-nativo-PS2
- https://github.com/ran-j/PS2Recomp

O runtime inclui contribuições e referências de Waffle565, YYOzcan/Tobiichi-Port e PCSX2, com avisos preservados nos arquivos. O backend de apresentação OpenGL veio da base Linux existente.

Os overrides de God of War II, seu mapa de funções, suas capturas de IOP/VU e seus endereços de patch não foram importados como configuração de Ben 10. O executável de entrada e o título do runtime foram adaptados para o novo projeto.

Uma exceção verificada é a ponte de funções do SDK de áudio LIBSD (GPL-3.0): a seção `.text` do módulo fornecido pelo disco de Ben 10 é idêntica à da base, com 19.376 bytes e SHA-256 `cc9151a9f6a446dfc1b9d2cae2db686b9b338af8162a74ca4905df33d156cb87`. Foram adaptadas somente essas funções de SDK. A ativação também exige tamanho e hash FNV do código relocacionado compatíveis; módulos de outros jogos não recebem hooks apenas pelo nome. Os módulos binários não são distribuídos.

Para a primeira prova de execução local, foram reutilizadas bibliotecas estáticas genéricas e o GCC portátil do kit de GodOfWar2PC; `libbase.a`, que contém patches específicos de God of War II, foi excluída. O script de build público recompila as bibliotecas a partir do código incluído.

O projeto https://github.com/ExtremestoneGG/Cosmic-Recompi foi consultado; ele tem como alvo Xbox 360/Windows e não foi incorporado a esta implementação PS2.

O helper de testes sintéticos `tests/support/iop_compat_test_support.h` também vem da base PS2Recomp/GodOfWar2PC (GPL-3.0). Os testes não contêm código ou arquivos de jogos.
