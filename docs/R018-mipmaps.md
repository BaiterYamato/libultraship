# R018 — mipmaps e anisotropia no Fast3D D3D11

## Escopo e integração

Todas as gravações desta entrega ficam nesta pasta. Os cinco arquivos fornecidos
eram byte a byte iguais aos originais no início. O original em
`D:/Desenvolvimento/ship-lua-worktrees/GFX-UNIFY-001-host/libultraship` foi somente lido.
Sem rede, instalação, commit ou alteração do estado Git.

Nesta revisão, `gfx_direct3d_common.cpp` tem somente um include condicionado: o
gerador real é `gfx_direct3d_common_build_shader` no final de
`gfx_direct3d11.cpp`. Ele processa com Prism o recurso
`shaders/directx/default.shader.hlsl`. Por isso foi necessária uma cópia local
adicional desse template. `gfx_direct3d_common.cpp`, `gfx_direct3d11.h` e
`gfx_rendering_api.h` permanecem iguais aos originais.

`PATCH.diff` usa caminhos relativos à raiz de **libultraship**:

- `src/fast/backends/gfx_direct3d11.cpp`;
- `include/fast/backends/gfx_direct3d_common.h`;
- `src/fast/backends/mip_gen.h` e `mip_gen.cpp`;
- `src/fast/shaders/directx/default.shader.hlsl`;
- `tests/test_mip_gen.cpp`, `tests/test_hlsl.py`;
- `docs/R018-mipmaps.md` (estas notas).

Aplicação futura, a partir dessa raiz: `git apply --check PATCH.diff` e
`git apply PATCH.diff`. Nenhuma aplicação foi feita no original nesta tarefa.
É necessário reconfigurar o CMake: `src/fast/CMakeLists.txt` já coleta
`backends/*.cpp` com GLOB, mas sem CONFIGURE_DEPENDS.
Regerar também o pacote de shaders usado em runtime (`f3d.o2r`), incluindo o
template editado; atualizar apenas o executável deixa o shader antigo ativo.

## Escolha: mipmaps CPU

`GenerateMips` faz a redução, mas não oferece o controle necessário sobre a
cobertura dos testes de alpha do Fast3D. O gerador independente usa box por área
em RGBA8 UNORM, RGB ponderado pelo alpha e divisão posterior, para não misturar
o RGB de texels transparentes nas partes visíveis. Alpha contínuo recebe somente
a média por área. Não se introduziu conversão sRGB: o recurso original é UNORM.

Dimensões seguem `max(1, dimensão / 2)` até 1×1. Caixas com pesos fracionários
incluem a última linha/coluna de dimensões ímpares; 33×17 não vira uma imagem
32×16 recortada. O nível 0 não é alterado.

Texturas com alpha exclusivamente 0/255 e com ambos presentes são classificadas
como recorte. A referência é a cobertura da **base**, e não a do mip anterior.
Em cada nível, ordenar pela média de alpha e escolher o número inteiro mais
próximo de texels cobertos evita a deriva e resolve empates como checkerboards,
que um simples multiplicador global não consegue resolver. Empates usam um hash
espacial determinístico. Os escolhidos recebem alpha pelo menos 128; os demais,
no máximo 7. Assim a cobertura discreta é a mesma nos limiares reais
`texel.a > 0.19` e `texel.a >= 8/256`, e também no usual `>= 0.5`.

Garantia medida por nível: erro absoluto de cobertura <= `0.5 / número_de_texels`.
Essa tolerância é a quantização inevitável para o inteiro mais próximo. Um mip
1×1 só pode ter cobertura 0 ou 1; ele não pode representar geometricamente 37%.
As correções não realimentam a próxima redução: esta usa o nível sem correção.

O upload continua IMMUTABLE, somente SHADER_RESOURCE, com um subresource inicial
por nível e SRV completo; não adiciona bind RENDER_TARGET, GenerateMips nem UAV.

## CVars e amostragem

Leitura pelo mesmo padrão `Context -> GetConsoleVariables() -> GetInteger`
que já existia no arquivo:

| CVar | Padrão | Comportamento |
| --- | --- | --- |
| `gSettings.TextureMipmaps` | 1 | 0: upload de um nível, LOD 0, anisotropia desativada e 3-point original; diferente de 0: mipmaps elegíveis |
| `gSettings.TextureAnisotropy` | 8 | 1, 2, 4, 8 ou 16; outros valores arredondam para baixo entre esses limites |

As mudanças são observadas em StartFrame. Alterar mipmaps invalida o cache do
interpretador para reupload no próximo uso, sem recriar framebuffers; anisotropia
só muda o sampler. O upload consulta a CVar diretamente, inclusive antes do
primeiro StartFrame. Não há necessidade de limpar o cache de shaders.

- Jogo point, ou FILTER_NONE: MIN_MAG_MIP_POINT e MaxLOD 0.
- Jogo linear, modo FILTER_LINEAR, mundo: MIN_MAG_MIP_LINEAR em 1×;
  FILTER_ANISOTROPIC em 2×/4×/8×/16×, mip linear implícito no filtro anisotrópico.
- FILTER_THREE_POINT: mantém três taps point no nível 0 durante ampliação.
  Na redução usa Sample trilinear/anisotrópico com coordenadas não ajustadas
  aos texels. Essa é uma escolha explícita: não emula três taps triangulares
  separadamente em cada mip. Mantém a emulação perto da câmera e permite
  anisotropia real no chão/paredes distantes.
- Wrap/mirror/clamp continuam usando a mesma conversão por eixo, inclusive a
  precedência existente de CLAMP sobre MIRROR|CLAMP.

O template já usava Sample, sem Load/SampleLevel 0 nos caminhos de texturas do
mundo. O problema no 3-point era aplicar Sample **depois** de ajustar UV aos
texels: as derivadas dessas coordenadas podem zerar ou saltar. Agora Sample e
ddx/ddy usam UV anterior a esse ajuste; a maior derivada em espaço de texels
identifica redução. O Sample de redução é calculado antes da seleção do caminho.
Não é necessário calcular LOD explícito: Sample mantém as derivadas e deixa a
seleção de mip/footprint anisotrópico para o hardware.

Samplers point adicionais s8/s9 permitem que o 3-point de ampliação conserve
suas amostras exatas mesmo com anisotropia no sampler principal. Eles usam os
mesmos endereçamentos. O backend reserva s8..s13 para o conjunto de slots Fast3D;
o template usa s8/s9 para as duas texturas-base, inclusive máscaras/blends.
O cache de descritores evita criar samplers repetidamente em trocas HUD/mundo.
O uint `mipmaps` ocupa o antigo padding, preservando 16 bytes por textura no CB,
com static_asserts de tamanho e offset. Atualizações do CB consideram mudanças
de sampler/estado mesmo com a mesma SRV.

## HUD, framebuffer e riscos reais

Framebuffers, render targets, depth, MSAA, cópia e resolve mantêm os recursos de
um nível já existentes. IDs de framebuffer são marcados e nunca habilitam
mipmaps/anisotropia na amostragem. Não se gera cadeia sobre conteúdo GPU mutável.

Como UploadTexture não informa a finalidade de uma textura, a restrição de HUD
é **por draw**: sem teste de profundidade, usar LOD 0 e filtro legado. A mesma
textura pode alternar HUD/mundo sem reupload nem depender de troca da SRV.
Isso também exclui céu/efeitos sem depth test. HUD que excepcionalmente desenha
com depth test pode receber mipmaps; é necessária conferência em jogo.

A garantia de cobertura é sobre texels de cada mip, antes do sampler e combiner.
Interpolação bilinear/trilinear, anisotropia, alteração de alpha pelo combiner e
outros limiares podem mudar a cobertura **na tela**. Não há promessa de cobertura
exata por pixel em qualquer distância. Testar folhagem/grades em movimento.
O rank com desempate pode mudar a distribuição espacial e causar granulação;
coarse mips perdem a silhueta por limite de resolução. O corte 3-point/hardware
em footprint 1 pode apresentar uma pequena transição visual.

A classificação binária não sabe se uma textura será usada depois para blend:
nesse caso a projeção de alpha pode mudar sua opacidade. Texturas de recorte
com alpha originalmente contínuo não são corrigidas, para preservar transparência
suave. Não existe metadado de finalidade no contrato de upload fornecido; ampliar
essa classificação exige informação do interpretador. RGB de regiões totalmente
transparentes não é dilatado, portanto halos residuais do conteúdo original
precisam ser avaliados. HD/replacements grandes aumentam o custo de geração e
ordenação no upload; medir pausas e consumo antes de publicar.

## Memória

GPU RGBA8 sem considerar alinhamento do driver:

| Base | Cadeia em texels | Bytes base → cadeia | Aumento |
| --- | --- | --- | --- |
| 1×1 | 1 | 4 → 4 | 0% |
| 2×1 | 3 | 8 → 12 | 50% |
| 64×64 | 5461 | 16384 → 21844 | 33,33% |
| 33×17 | 732 | 2244 → 2928 | 30,48% |
| 1×33 | 64 | 132 → 256 | 93,94% |

Para quadrados grandes, cerca de +1/3; texturas muito estreitas se aproximam de
+100%. A CPU mantém a cadeia, o nível bruto e índices size_t do nível que está
sendo corrigido, temporariamente. Esses dados são liberados após o upload; não
há cópia permanente adicional dos pixels na estrutura TextureData. Cada textura
ganha metadados e uma referência de sampler; os objetos sampler são compartilhados
pelo cache do dispositivo. Desativar a CVar e reusar as texturas remove as cadeias
dessas texturas; IDs antigos sem novo uso continuam com recursos até o ciclo de
vida normal do cache, como no backend existente.

## Validação realizada e limites

Nesta pasta:

```powershell
g++ -std=c++17 -O2 -Wall -Wextra -Werror -pedantic mip_gen.cpp test_mip_gen.cpp -o test_mip_gen.exe
.\test_mip_gen.exe
python test_hlsl.py
```

CPU: 35 combinações de dimensão/padrão (1×1, 2×1, 64×64, 33×17, 1×33, 3×3,
7×2; opaco, checkerboard, grade, máscara irregular, transparente), cadeia completa,
nível 0 idêntico, cobertura nos três limiares dentro da tolerância por nível e
repetibilidade dos empates. Também média RGBA, alpha contínuo, RGB transparente,
última linha/coluna NPOT e rejeição de entradas inválidas. Todos passaram;
compilação g++ sem warnings com -Werror.

HLSL: D3DCompiler_47.dll disponível; 32 fixtures e 64 compilações VS/PS 4.0,
O2 e warnings como erros, passaram. Cobrem linear/3-point, uma/duas texturas,
máscaras/blends e clamp em nenhum/S/T/ambos os eixos. `hlsl_examples/` contém os
HLSLs e bytecodes. O script extrai helper e corpo de amostragem do template,
expande apenas esse subconjunto de diretivas em fixtures com recursos e combiner
simplificados. **Não executa Prism nem o combiner N64 completo.**

Após aplicar no repo, os comandos CPU usam `-Isrc/fast/backends`,
`src/fast/backends/mip_gen.cpp` e `tests/test_mip_gen.cpp`;
o script `python tests/test_hlsl.py` também encontra o template no caminho real.

O patch foi validado com git apply --check sobre uma árvore local que reproduz
os originais; os originais foram revalidados por SHA-256 sem alteração. Arquivos
`validation.json` e `VALIDACAO.txt` registram os resultados desta entrega.
Sem MSVC: não foi compilado/linkado o renderer inteiro nem executado o jogo.
Essas verificações não comprovam a aparência, desempenho ou ausência de avisos
da camada de debug D3D11 em runtime. Não houve teste D3D12.

## O que testar no jogo

1. Rebuild/reempacotar shader e limpar shaders antigos: chão/paredes inclinados,
   aproximação/afastamento e rotação de câmera em 1×/2×/4×/8×/16×.
2. Folhagem/grades contra fundos claros/escuros, incluindo mips pequenos,
   transições de LOD e alpha após combiner; comparar área aparente e cintilação.
3. Point permanece nítido; linear e 3-point preservam ampliação, inclusive
   minificação anisotrópica e a transição em footprint 1.
4. Wrap/mirror/clamp separados por eixo, NPOT 33×17, 2×1 e 1×1, máscaras/blends
   e replacements; conferir costuras e contaminação por bordas transparentes.
5. HUD, texto, ícones, menus, sprites 2D e a sequência mundo → HUD → mundo com
   a mesma textura, particularmente HUD com teste de profundidade.
6. Framebuffers de câmera/pausa, telas copiadas, resize, MSAA/resolve e passes
   de fog/postprocess, com camada de debug sem hazards/slots inválidos.
7. Alternar TextureMipmaps 1→0→1 com texturas já em cache; em 0 comparar com
   a versão original (oito casos: point/linear/3-point, HUD/mundo e framebuffer),
   incluindo mudança de anisotropia enquanto mipmaps está desligado.
8. Medir tempo de upload, frame time e memória em áreas com muitas texturas e
   packs HD; a validação CPU e a compilação de shaders não substituem esse teste.
