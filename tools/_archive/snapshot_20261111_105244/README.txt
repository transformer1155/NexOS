=== NexOS dev env snapshot 11/11/2026 10:52:46 ===
Host OS: Windows 11 家庭版 (Build 26300)
CPU: AMD Ryzen 7 H 255 (AuthenticAMD)
Project root: e:\NexOS\NexOS
VBox Linux VM: {c625da01-e281-4095-9500-63858447e095}
SSH tunnel: 127.0.0.1:2222 (transform1155@Ubuntu build VM)
SSH key: C:\Users\transform1155\.ssh\id_rsa
Remote repo: /media/sf_NexOS/NexOS
vm-run.bat location: C:\windows\vm-run.bat

=== Critical discovery today ===
ROOT CAUSE OF NESTED SVM FAILURE:
  Windows Hypervisor (hypervisorlaunchtype=auto) 常驻
  → VBox NEM backend 被迫使用 (HypervisorPresent=TRUE)
  → NEM 天生不支持 nested HW virtualization
  → VBox 给 guest CPUID 手动清掉 SVM bit
  
FIX: fix_nested_virt.bat (need admin cmd + Windows reboot)
  bcdedit /set hypervisorlaunchtype off
  dism /online /disable-feature Microsoft-Hyper-V-All /norestart
  dism /online /disable-feature VirtualMachinePlatform /norestart

=== After reboot, expected state ===
  VBox hostinfo: nested HW virtualization: YES
  Guest /proc/cpuinfo: svm flag present
  /dev/kvm: accessible (after sudo modprobe kvm_amd)
  qemu -accel kvm -cpu host (auto-detected by vm-run)
