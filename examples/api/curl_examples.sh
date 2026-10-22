#!/bin/sh
# The Patina HTTP API with curl. Start a server first:
#   patina --headless --http                  (this machine, no token)
#   patina --headless --lan                   (local network; token in the log / settings folder)
URL=${PATINA_URL:-http://127.0.0.1:7870/api/v1}
AUTH=${PATINA_TOKEN:+-H "Authorization: Bearer $PATINA_TOKEN"}

curl -s $URL/health
# one command, answered when it is done
curl -s $AUTH -X POST $URL/commands/new_project -H 'Content-Type: application/json' \
     -d '{"primitive": "sphere", "resolution": 1024}'
# upload a mesh into the workspace
curl -s $AUTH -X PUT --data-binary @crate.glb $URL/files/in/crate.glb
# a job: queued, runs in the background, reports progress
curl -s $AUTH -X POST $URL/jobs -H 'Content-Type: application/json' -d '{
  "name": "crate",
  "steps": [
    {"cmd": "open_file",       "args": {"path": "in/crate.glb"}},
    {"cmd": "bake_mesh_maps",  "args": {"ao": true, "curvature": true}},
    {"cmd": "apply_smart_material", "args": {"id": "patina_smart_rusted_iron"}},
    {"cmd": "export_textures", "args": {"directory": "out/crate", "preset": "gltf"}}
  ]}'
# -> {"id": "j1-...", "status": "queued", ...}; wait up to 60 s for it:
#   curl -s $AUTH "$URL/jobs/j1-...?wait=60"
# download the results
#   curl -s $AUTH "$URL/files?dir=out/crate"
#   curl -s $AUTH -o crate_baseColor.png $URL/files/out/crate/crate_baseColor.png
