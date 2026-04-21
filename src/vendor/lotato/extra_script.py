Import("env")

# Lotato includes firmware headers (e.g. MeshCore.h, helpers/AdvertDataHelpers.h) from this repo.
env.Append(CPPPATH=["$PROJECT_DIR/src"])
