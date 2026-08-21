from pathlib import Path
import shutil

Import("env")

project_dir = Path(env["PROJECT_DIR"])
web_dir = project_dir / ".pio" / "web"
web_dir.mkdir(parents=True, exist_ok=True)

for asset in ("index.html", "bg.webp", "crop.webp"):
    shutil.copy2(project_dir / asset, web_dir / asset)
