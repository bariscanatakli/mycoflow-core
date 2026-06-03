# Lisans Bitirmesi Tezi Template

Basit ve temiz lisans bitirmesi (Bachelor's Thesis) için LaTeX template'i.

## Kurulum ve Derleme

### Gereklilikler
- LaTeX (pdfTeX, XeTeX veya LuaTeX)
- Biber (kaynakçalar için)
- Make (opsiyonel, derleme için)

### Derleme

**Terminal ile:**
```bash
pdflatex main.tex
biber main
pdflatex main.tex
pdflatex main.tex
```

**Make ile:**
```bash
make
```

**Temizleme:**
```bash
make clean
```

## Logo Ayarı 🎓

Logo dosyası zaten `logos/image.png` olarak eklidir. Muğla Sıtkı Koçman Üniversitesi logosunu göstermektedir.

Eğer kendi üniversitenizin logosunu kullanmak istiyorsanız:
1. Logo dosyasını `logos/image.png` adıyla kaydedin
2. Template otomatik olarak kabul edecektir

## Dosya Yapısı

```
.
├── main.tex                 # Ana dosya - TÖDÖLERİ BURADAN BAŞLAYIN
├── settings.tex             # Paketler ve stil ayarları
├── bibliography.bib         # Kaynakçalar (BibTeX formatı)
├── chapters/
│   └── 01_introduction.tex  # Giriş bölümü
├── pages/
│   ├── cover.tex           # Kapak sayfası
│   ├── title.tex           # Başlık sayfası
│   ├── abstract.tex        # Özet
│   └── appendix.tex        # Ek
├── figures/                # Resimler ve grafikler
└── logos/                  # Üniversite logosu (mu-logo.pdf)
```

## Başlangıç - main.tex'i Düzenleyin

`main.tex` dosyasındaki bu kısımları düzenleyin:

```latex
\newcommand*{\getUniversity}{Muğla Sıtkı Koçman University}
\newcommand*{\getFaculty}{Faculty/Department}
\newcommand*{\getTitle}{Tez Başlığınız}
\newcommand*{\getAuthor}{Adınız Soyadınız}
\newcommand*{\getDoctype}{Bachelor's Thesis}
\newcommand*{\getSupervisor}{Danışman Adı}
\newcommand*{\getAdvisor}{Öğretim Görevlisi Adı}
\newcommand*{\getSubmissionDate}{Month Year}
\newcommand*{\getSubmissionLocation}{Istanbul}
```

## Bölüm Ekleme

Yeni bölüm dosyası oluşturun (örn. `chapters/02_methodology.tex`) ve main.tex'e ekleyin:

```latex
\input{chapters/02_methodology}
\input{chapters/03_results}
\input{chapters/04_conclusion}
```

## Kaynakçalar

Kaynakları `bibliography.bib` dosyasına BibTeX formatında ekleyin:

```bibtex
@article{Author2020,
  author = {Author, A.},
  title = {Title of the article},
  journal = {Journal Name},
  year = {2020}
}
```

## Özet (Abstract)

`pages/abstract.tex` dosyasını düzenleyin.

## Ekler (Appendix)

`pages/appendix.tex` dosyasını düzenleyin.

## Resim Ekleme

Resimlerinizi `figures/` klasörüne koyun ve bölümlerde referans verin:

```latex
\begin{figure}
  \centering
  \includegraphics[width=0.8\textwidth]{figures/myimage.png}
  \caption{Resim başlığı}
  \label{fig:myimage}
\end{figure}
```

## Tablo Ekleme

```latex
\begin{table}
  \centering
  \begin{tabular}{lcc}
    \toprule
    Başlık 1 & Başlık 2 & Başlık 3 \\
    \midrule
    Satır 1  & Veri 1   & Veri 2   \\
    Satır 2  & Veri 3   & Veri 4   \\
    \bottomrule
  \end{tabular}
  \caption{Tablo başlığı}
  \label{tab:mytable}
\end{table}
```

## Kod Bloğu Ekleme

```latex
\begin{lstlisting}[language=Python, caption={Python örneği}]
def hello():
    print("Hello, World!")
\end{lstlisting}
```

## Yardım

- LaTeX hakkında daha fazla bilgi: [LaTeX Wikibook](https://en.wikibooks.org/wiki/LaTeX)
- LaTeX soruları: [TeX StackExchange](https://tex.stackexchange.com/)
- Online editör: [Overleaf](https://www.overleaf.com/)

## Attribution (Atıf)

Bu template, aşağıdaki orijinal çalışmalardan uyarlanmıştır:

- **Orijinal Kaynak:** [tum-thesis-latex](https://github.com/fwalch/tum-thesis-latex) by Florian Walch ve kontribütörler
- **TUM Informatik Sürümü:** [TUM Thesis for Informatics Template](https://www.overleaf.com/latex/templates/tum-thesis-for-informatics-template/ttjnzgmgvvxj) by Tobias Weiher
- **Lisans:** Creative Commons CC BY 4.0

Bu template Muğla Sıtkı Koçman Üniversitesi lisans tezleri için sadeleştirilmiş sürümüdür.

---

**Başarılar!** 🎓
