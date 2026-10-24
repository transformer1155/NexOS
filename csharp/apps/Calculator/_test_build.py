
import os, subprocess

home = r"C:\Users\transform1155"
os.environ["USERPROFILE"] = home
os.environ["APPDATA"] = os.path.join(home, "AppData", "Roaming")
os.environ["LOCALAPPDATA"] = os.path.join(home, "AppData", "Local")
os.environ["HOMEDRIVE"] = "C:"
os.environ["HOMEPATH"] = r"\Users\transform1155"
os.environ["PROGRAMDATA"] = r"C:\ProgramData"
os.environ["SystemRoot"] = r"C:\Windows"
os.environ["PATH"] = r"C:\Program Files\dotnet;C:\Windows\System32;C:\Windows" + os.pathsep + os.environ.get("PATH", "")

dotnet = r"C:\Program Files\dotnet\dotnet.exe"
os.chdir(r"E:\NexOS\NexOS\csharp\apps\Calculator")

print("== dotnet restore Calculator ==")
rc = subprocess.call([dotnet, "restore", "Calculator.csproj"])
print("restore exit:", rc)

print("\n== dotnet build Calculator ==")
rc = subprocess.call([dotnet, "build", "Calculator.csproj", "-c", "Release", "-v", "minimal"])
print("build exit:", rc)

# 找 DLL
for root, dirs, files in os.walk("bin"):
    for f in files:
        if f.endswith(".dll"):
            full = os.path.join(root, f)
            print("\nDLL found:", full, os.path.getsize(full), "bytes")
