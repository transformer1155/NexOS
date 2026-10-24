
import os, subprocess

dotnet = r"C:\Program Files\dotnet\dotnet.exe"
os.chdir(r"E:\NexOS\NexOS\test_app")

# 1. restore via pwsh
print("== restore via pwsh ==")
rc = subprocess.call(["powershell", "-NoProfile", "-Command", 
                      r"& 'C:\Program Files\dotnet\dotnet.exe' restore test_app.csproj"])
print("rc:", rc)

# 2. build
print("\n== build via pwsh ==")
rc = subprocess.call(["powershell", "-NoProfile", "-Command",
                      r"& 'C:\Program Files\dotnet\dotnet.exe' build test_app.csproj -c Release -v normal"])
print("rc:", rc)

# 3. 深度搜所有产物
print("\n== 深度搜索 dll/mex/exe ==")
for root_dir, dirs, files in os.walk("."):
    for f in files:
        if f.endswith((".dll", ".mex", ".exe", ".bin")) and "obj" not in root_dir:
            full = os.path.join(root_dir, f)
            print("  " + full + " (" + str(os.path.getsize(full)) + ")")
