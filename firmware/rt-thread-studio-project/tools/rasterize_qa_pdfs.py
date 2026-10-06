from pathlib import Path

import pypdfium2 as pdfium


PROJECT = Path(r"D:\RT-ThreadStudio\workspace\test_pro3_v8_dbui_kneeout")
BASE = next(p for p in PROJECT.iterdir() if p.is_dir() and p.name.startswith("Word"))
PDFS = sorted(BASE.glob("qa_*/*.pdf"))


for pdf_path in PDFS:
    document = pdfium.PdfDocument(pdf_path)
    for index in range(len(document)):
        page = document[index]
        bitmap = page.render(scale=150 / 72)
        image = bitmap.to_pil()
        image.save(pdf_path.parent / f"page-{index + 1}.png")
        page.close()
    print(f"{pdf_path}: {len(document)} pages")
    document.close()
