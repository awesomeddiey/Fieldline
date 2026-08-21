from pathlib import Path
import shutil
import sys

Import("env")

project_dir = Path(env["PROJECT_DIR"])
web_dir = project_dir / ".pio" / "web"
web_dir.mkdir(parents=True, exist_ok=True)

for asset in ("index.html", "bg.webp", "crop.webp"):
    shutil.copy2(project_dir / asset, web_dir / asset)

# Regenerate the embedded dashboard (web_assets.h) from index.html + images.
sys.path.insert(0, str(project_dir / "scripts"))
import gen_web_assets
gen_web_assets.generate()
