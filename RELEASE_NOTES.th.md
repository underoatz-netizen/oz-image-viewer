# บันทึกประจำรุ่น PikaView 0.1.2 (เดิมชื่อ OZ Image Viewer)

PikaView 0.1.2 เป็นรุ่นล่าสุดสำหรับระบบปฏิบัติการ Windows 64-บิต (x64) ของ PikaView (เดิมชื่อ OZ Image Viewer) ที่คอมไพล์ไฟล์ไบนารี PE (`ozviewer.exe` ตรงกัน 2 ทาง; `heif.dll` และ `libde265.dll` ตรงกัน 3 ทาง) แบบสะอาดจากซอร์สโค้ดและพิสูจน์ความตรงกันระดับบิตต่อบิต ขณะที่ตัวห่อหุ้มแพ็กเกจเผยแพร่ (`pikaview-0.1.2-win-x64-installer.exe` และ `pikaview-0.1.2-win-x64-portable.zip`) ถูกแพ็กครั้งเดียวจากชุดผลลัพธ์ Build A และผูกค่า SHA-256 ไว้ใน `SHA256SUMS` พัฒนาด้วย C++23, Win32, Direct3D 11, DXGI และ Direct2D โดยความสามารถหลักด้านการดูภาพ การเลื่อนดูภาพในโฟลเดอร์ การแก้ไขแบบไม่ทำลายต้นฉบับ (หมุน พลิก ครอป ปรับขนาด เลิกทำ/ทำซ้ำ) การพิมพ์ภาพเดี่ยวแบบโต้ตอบ (`PrintDlgExW`) การบันทึก/ส่งออก และการถอดรหัส HEIC/HEIF แบบไดนามิกยังคงอยู่ครบถ้วน พร้อมไอคอนแอป PikaView ใหม่ แถบคำสั่งแถวเดียวสูง 40 DIP ด้วย Lucide 1.71.0 การแก้ไขปัญหาแคช/พรีโหลดเมื่อบันทึก และคำแนะนำข้อผิดพลาดพื้นที่เก็บข้อมูล

ชื่อไฟล์โปรแกรมยังคงเป็น `ozviewer.exe` ข้อมูลระบุตัวตนภายในที่ใช้สำหรับการอัปเกรดและการลงทะเบียนยังคงเดิม ได้แก่ AppId ที่คงที่ ProgID `OZImageViewer.Image` โครงสร้างรีจิสทรี `HKCU\Software\OZImageViewer` และพาธติดตั้งต่อผู้ใช้ `%LOCALAPPDATA%\Programs\OZ Image Viewer` ซึ่งรักษาความต่อเนื่องเมื่ออัปเกรดทับทั้งรุ่น `0.1.0` และ `0.1.1` ส่วนไฟล์รุ่น แท็ก git และบันทึก provenance เชิงประวัติศาสตร์ของ `v0.1.0` และ `v0.1.1` ยังคงเก็บรักษาไว้ตามเดิมโดยไม่แก้ไขหรือเปลี่ยนเลขรุ่น

## ข้อมูลต้นทางและการสร้าง (Source & Build Identity)

- **Source commit:** `4541ffedc4fde9d4095d92f3f7a0c5397bbb501b` (working tree สะอาด, แท็ก `v0.1.2`)
- **Git tree:** `0c464561de3d418b8d2a8b3689ca163a1cbad7bd`
- **ค่าการสร้าง:** `Release` x64 โดยตั้งค่า `OZVIEWER_PRINT_DIAGNOSTICS=OFF`
- **แฟล็กการสร้างแบบกำหนดได้แน่นอน (Deterministic flags):** MSVC `/experimental:deterministic /Brepro /PDBALTPATH:%_PDB%`, ทริปเล็ต `oz-x64-windows-static-md` (`VCPKG_PLATFORM_TOOLSET=v143`, `/experimental:deterministic`, `libheif`/`libde265` ใช้ `/Brepro /PDBALTPATH:heif.pdb` และ `libde265.pdb`) และใช้ `file-local` named namespace `oz_viewer_main_internal` ใน `src/viewer/main.cpp`
- **เครื่องมือและทริปเล็ต:** `MSVC 14.44.35228.0`, `CMake 3.31.6-msvc6`, `vcpkg` baseline `52d80838fb40c755b1615fbc9c7b994a33742a22`, ทริปเล็ต `oz-x64-windows-static-md`

## สิ่งที่เปลี่ยนแปลงในรุ่น 0.1.2

- **ไอคอนแอป PikaView ใหม่:** ใช้งานอย่างสอดคล้องกันทั้งในทรัพยากรไฟล์ `ozviewer.exe` หน้าต่างหลัก กล่องโต้ตอบ หน้าจอว่าง/ต้อนรับ และตัวติดตั้ง Windows Setup
- **แถบคำสั่งแถวเดียวสูง 40 DIP ด้วย Lucide 1.71.0 และ UI โทนสีเทาเข้ม:** แทนที่แถบเมนูแบบเดิมด้วยแถบคำสั่งแถวเดียวสูง 40 DIP (ปุ่มขนาด `32` DIP เส้นคั่นแบบเรียบ เมนูส่วนเกิน **More** ด้านขวาที่มีลำดับแน่นอน ไอคอนแถบครอปที่คมชัด การเปิดเมนูด้วยคีย์บอร์ด `F10`/`Alt` คงคีย์ลัดเดิมทั้งหมด และคืนโฟกัสกลับสู่พื้นที่ภาพอัตโนมัติ) พร้อมเพิ่มประกาศ Lucide (ISC) และ Feather (MIT) ใน `THIRD-PARTY-NOTICES.md`
- **การล้างแคช/พรีโหลดเมื่อบันทึกและคำแนะนำข้อผิดพลาดพื้นที่เก็บข้อมูล:** แก้ไขสถานะแคช/พรีโหลดค้างเมื่อสั่งบันทึก ทำให้การสลับดู `Save -> Next -> Previous -> Save` โหลดและบันทึกซ้ำได้ถูกต้องโดยไม่เกิดข้อความผิดพลาดเท็จว่า "source file modified"; ประสานการบันทึกเสร็จสิ้นกับ checkpoint ของเอกสารปัจจุบันและการจัดตารางนำทางที่ค้างอยู่; และเพิ่มคำแนะนำภาษาอังกฤษ/ภาษาไทยสำหรับสื่ออ่านอย่างเดียวที่ป้องกันการเขียน (`win32 19`) รวมถึงกรณีถูกปฏิเสธสิทธิ์ (`win32 5`)
- **ความต่อเนื่องของการอัปเกรดในตำแหน่งเดิมข้ามรุ่น 0.1.0, 0.1.1 และ 0.1.2:** การติดตั้ง PikaView 0.1.2 ทับการติดตั้ง OZ Image Viewer 0.1.0 หรือ PikaView 0.1.1 ที่เป็นเจ้าของอยู่ จะอัปเกรดในโฟลเดอร์ `%LOCALAPPDATA%\Programs\OZ Image Viewer` โดยยังคง `ozviewer.exe` ค่า AppId ที่คงที่ ProgID `OZImageViewer.Image` และ `HKCU\Software\OZImageViewer` และคงค่าเริ่มต้นการเปิดไฟล์ของ Windows (`UserChoice`) ไว้ทั้งหมด
- **การสร้างแบบกำหนดได้แน่นอนด้วย MSVC `/Brepro` / `/experimental:deterministic`:** ใช้ทริปเล็ต `v143` (`oz-x64-windows-static-md`) ที่ปักหมุดไว้ร่วมกับการสร้าง dependency ใหม่สะอาดจากซอร์สโค้ด (`VCPKG_BINARY_SOURCES=clear`) เพื่อให้ได้ไบนารี PE ที่สร้างซ้ำได้ตรงกันทุกบิต

## แพ็กเกจสำหรับติดตั้งและใช้งาน

- `pikaview-0.1.2-win-x64-installer.exe` (4,276,637 ไบต์, ~4.1 MB): ตัวติดตั้งสำหรับ Windows แบบต่อผู้ใช้ ติดตั้งลงใน `%LOCALAPPDATA%\Programs\OZ Image Viewer` โดยไม่ต้องใช้สิทธิ์ผู้ดูแลระบบ (Admin) การเชื่อมโยงนามสกุลไฟล์และการสร้างชอร์ตคัตบนเดสก์ท็อปเป็นตัวเลือกที่ยังไม่ถูกเลือกไว้ก่อน (unchecked) และไม่เปลี่ยนค่าเริ่มต้นของระบบโดยพลการ ถอนการติดตั้งได้หมดจดผ่าน Windows Settings > Installed Apps
- `pikaview-0.1.2-win-x64-portable.zip` (2,744,297 ไบต์, ~2.6 MB): ไฟล์ ZIP แบบพกพาสำหรับแตกไฟล์ลงโฟลเดอร์ใดก็ได้ ประกอบด้วย `ozviewer.exe` พร้อม DLL รันไทม์ (`heif.dll`, `libde265.dll` และ VC143 CRT DLLs) รวมถึง `BUILD-PROVENANCE.json` และข้อความสัญญาอนุญาต
- `pikaview-0.1.2-lgpl-corresponding-source.zip` (2,421,056 ไบต์, ~2.3 MB): ซอร์สโค้ดต้นทางฉบับสมบูรณ์ สูตรและแพตช์ vcpkg ที่ใช้จริง `triplets/oz-x64-windows-static-md.cmake` ข้อความสัญญาอนุญาต LGPL-3.0 และ GPL-3.0 ฉบับเต็ม `README-BUILD-LGPL.txt` และ `build-lgpl-dlls.ps1` สำหรับส่วนประกอบ LGPL ที่โหลดแบบไดนามิก (`libheif` 1.23.2 และ `libde265` 1.1.1) ตามเงื่อนไข LGPLv3 §4 และ GPLv3 §6(d)
- `SHA256SUMS` (316 ไบต์): ไฟล์บันทึกค่าแฮชตรวจสอบความถูกต้องทางการของไฟล์ดาวน์โหลด

## ค่าแฮชตรวจสอบความถูกต้อง (SHA-256)

```
a36d9c8fff84a7374d8a5836b067398c0ab74a8ed578943d56b0b09eb9354bac *pikaview-0.1.2-win-x64-installer.exe
aa572e5eeab05c89fdcd5d92c734f75a21358254b9e4c4b32020df05ca8bd6d4 *pikaview-0.1.2-win-x64-portable.zip
f28505ee05f9dc9468e2da6ed8d4fc56d21c227ef6ef354a053ebbd551d32753 *pikaview-0.1.2-lgpl-corresponding-source.zip
```

ไฟล์ `SHA256SUMS` เองมีค่า SHA-256 คือ `bc3eff3d48b9fc6cda8dc01d55cdfec49ad05f6aee5d1276b0c5872c3d73fd27` (316 ไบต์)

ตรวจสอบไฟล์ที่ดาวน์โหลดด้วย PowerShell (`Get-FileHash -Algorithm SHA256 filename`) หรือ Command Prompt (`certutil -hashfile filename SHA256`) ก่อนเปิดใช้งาน

## สถานะลายเซ็น Authenticode และคำแนะนำ SmartScreen

- **ไฟล์ไบนารีของโปรเจกต์ที่ยังไม่ได้ลงลายเซ็น (`NotSigned`):**
  - `pikaview-0.1.2-win-x64-installer.exe`: 4,276,637 ไบต์, SHA-256 `a36d9c8fff84a7374d8a5836b067398c0ab74a8ed578943d56b0b09eb9354bac`
  - `ozviewer.exe`: 3,853,824 ไบต์, SHA-256 `04dfd470ae8ab01f4e911d15dd4f38a85b4e18ebc0fd0442dfcfda10ff036c92`
  - `heif.dll`: 1,423,872 ไบต์, SHA-256 `9bf0954c60cfb0e16ba55c329f169657364e88ca0643d5cdc978bc4253a8d019`
  - `libde265.dll`: 366,592 ไบต์, SHA-256 `15536336f0ef6ca8b5249a853685e90b1414f5996080525b52e7c9d268faadcb`
- **Microsoft CRT DLL ที่ลงลายเซ็นถูกต้อง (`Valid`):**
  - `msvcp140.dll`: 557,728 ไบต์, SHA-256 `0f885b509a685d2bbfa652fed26b5fb31d88fbdab0a978c641d1c7b8aa460aa9`
  - `msvcp140_atomic_wait.dll`: 50,304 ไบต์, SHA-256 `640b2aefced484d0368eea5bdd06addd0658a3a70a49256e560d6923b404a479`
  - `vcruntime140.dll`: 124,544 ไบต์, SHA-256 `d5e4d9a3e835fa679450145d6a7d94e36573a509317111904d9b3712c30d9066`
  - `vcruntime140_1.dll`: 49,792 ไบต์, SHA-256 `1f2d41c4aa5db0bc33ebf7b66d72943a817d7ce6cbe880502a9403823633093f`
- **คำแนะนำสำหรับ Windows SmartScreen และเบราว์เซอร์:** เนื่องจากตัวติดตั้งและไฟล์โปรแกรมหลักยังไม่ได้ลงลายเซ็นดิจิทัล (`NotSigned`) Windows SmartScreen หรือเบราว์เซอร์อาจแสดงคำเตือน "Unknown Publisher" ก่อนเปิดไฟล์ โปรดตรวจสอบว่าดาวน์โหลดมาจาก repository ทางการบน GitHub (`https://github.com/underoatz-netizen/oz-image-viewer`) และตรวจค่า SHA-256 ให้ตรงกับ `SHA256SUMS` และรายการด้านบนก่อนรันไฟล์ทุกครั้ง

## หลักฐานการสร้างซ้ำได้ตรงกันและการสร้างสะอาดจากซอร์สโค้ด

- **การสร้างสะอาดจากซอร์สโค้ดสำหรับแอปเต็ม 2 ทางและส่วนประกอบ LGPL 3 ทาง:** การสร้างแอปพลิเคชันฉบับเต็มแบบสะอาดและเป็นอิสระสองครั้ง (`repro-a` และ `repro-b` ที่มีโฟลเดอร์และเวลาต่างกัน) ถูกคอมไพล์ใหม่จากซอร์สโค้ดทั้งหมดโดยตั้งค่า `VCPKG_BINARY_SOURCES=clear` และไม่มีการคืนค่าไบนารีจากแคช (ยืนยันความตรงกัน 2 ทางของ `ozviewer.exe`) พร้อมการสร้างเฉพาะส่วนประกอบ LGPL แบบแยกอิสระครั้งที่ 3 ด้วย `build-lgpl-dlls.ps1` ที่แตกออกจาก `pikaview-0.1.2-lgpl-corresponding-source.zip` (ตั้งค่า `VCPKG_BINARY_SOURCES=clear` เช่นกัน เพื่อยืนยันความตรงกัน 3 ทางของ `heif.dll` และ `libde265.dll`)
- **หลักฐานความตรงกันระดับบิตต่อบิต (`docs/releases/v0.1.2/REPRODUCIBLE-BUILD-PROOF.json`):**
  - `ozviewer.exe` (3,853,824 ไบต์): `04dfd470ae8ab01f4e911d15dd4f38a85b4e18ebc0fd0442dfcfda10ff036c92` (`repro-a` == `repro-b`, **ตรงกัน 100% ทุกบิต**)
  - `heif.dll` (1,423,872 ไบต์): `9bf0954c60cfb0e16ba55c329f169657364e88ca0643d5cdc978bc4253a8d019` (`repro-a` == `repro-b` == Standalone LGPL rebuild, **ตรงกัน 100% ทุกบิต**)
  - `libde265.dll` (366,592 ไบต์): `15536336f0ef6ca8b5249a853685e90b1414f5996080525b52e7c9d268faadcb` (`repro-a` == `repro-b` == Standalone LGPL rebuild, **ตรงกัน 100% ทุกบิต**)
  - ไบนารี PE ที่คอมไพล์ทั้งหมดมี debug directory แบบ `repro` และไม่มีพาธ PDB ของเครื่องผู้สร้าง (`heif.pdb` / `libde265.pdb`)
- **ความเชื่อมโยงกับ provenance ร่วม:** ข้อความคงที่ใน `claimLimits` บรรทัดที่ 39 ของ `docs/releases/v0.1.2/BUILD-PROVENANCE.json` อธิบายขอบเขตของไฟล์ `BUILD-PROVENANCE.json` เพียงลำพัง ส่วนข้อกำหนดคู่ขนานทั้งสองรายการ (หลักฐานการสร้างซ้ำได้ตรงกันและการสร้าง dependency ใหม่แบบสะอาดแยกขาดในเวิร์กสเปซใหม่) ได้รับการยืนยันครบถ้วนสำหรับรุ่น v0.1.2 ใน `docs/releases/v0.1.2/REPRODUCIBLE-BUILD-PROOF.json` ทั้งนี้ตัวห่อหุ้มแพ็กเกจเผยแพร่ (เวลาในไฟล์ `pikaview-0.1.2-win-x64-portable.zip` และตัวห่อหุ้ม Inno Setup `pikaview-0.1.2-win-x64-installer.exe`) ถูกแพ็กครั้งเดียวจากชุดผลลัพธ์ Build A ที่ตรวจสอบแล้วและผูกค่า SHA-256 ไว้ใน `SHA256SUMS` (ขณะที่ `BUILD-PROVENANCE.json` บันทึกค่าแฮชของไฟล์ไบนารีที่คอมไพล์แล้ว VC143 CRT DLLs ที่ลงลายเซ็น และแพ็กเกจ `pikaview-0.1.2-lgpl-corresponding-source.zip`)

## ผลการตรวจสอบที่สังเกตได้จริงและข้อจำกัดของหลักฐาน

- **ชุดทดสอบ CTest เฉพาะเป้าหมายบน Clean Build A และชุดทดสอบแพ็กเกจ/provenance (`OZVIEWER_PRINT_DIAGNOSTICS=OFF`):**
  - Clean Build A CTest เฉพาะเป้าหมายผ่าน `11/11` รายการ (`ozviewer_core_tests` 2987/0, `ozviewer_stage7_seam_tests` 5366/0, `ozviewer_localization_tests` 1249/0, `ozviewer_recycle_delete_tests` 27/0, `ozviewer_icon_asset_tests`, `release_build_wrapper_tests` และสแกนแยก/แคตตาล็อก 5 รายการ; ทั้งนี้ยังไม่ได้รันชุดทดสอบเต็ม 52 รายการซ้ำหลังคอมมิต `4541ffe`)
  - ชุดทดสอบแพ็กเกจ provenance และการย้ายรุ่นผ่านทั้งหมดด้วย exit code `0`: `validate-portable-payload.ps1`, `smoke-portable.ps1` (`FirstPresent` PASS), `test-installer-migration.ps1` (จำลอง `0.1.0`/`0.1.1 -> 0.1.2` + คอมไพล์ `ISCC.exe`), `test-dependency-provenance.ps1` และ `test-envelope-provenance.ps1`
- **Clean-room GitHub Actions CI (run `44501854397` บน `windows-2022` x64):**
  - ผ่าน `fetch-candidate` (47 วินาที) และ `validate-candidate` (1 นาที 26 วินาที): ชุดทดสอบ disposable lifecycle ผ่าน 48 รายการ (47 `pass`, คำเตือน `non_admin_integrity_check` ตามคาด 1 รายการ, และ 0 `fail`) และเกต `standard-user-lifecycle` ผ่าน 12/12 รายการฝั่ง parent + 48/48 รายการฝั่ง child ในบัญชีผู้ใช้ทั่วไปแบบไม่ยกระดับสิทธิ์
  - ครอบคลุมการติดตั้งใหม่ต่อผู้ใช้ `FirstPresent` benchmark smoke การเชื่อมโยงนามสกุลไฟล์และชอร์ตคัตบนเดสก์ท็อปแบบเลือกเอง การอัปเกรดในตำแหน่งเดิม `v0.1.0 -> v0.1.2` ที่โฟลเดอร์ติดตั้งเดิม แฮชไฟล์รันไทม์ทั้งหมด การคงค่า `UserChoice` ครบทั้ง 14 นามสกุลไฟล์ และการถอนการติดตั้งอย่างหมดจด
- **ข้อจำกัดของหลักฐานตามจริง:**
  - เส้นทางอัปเกรด `0.1.1 -> 0.1.2` ผ่านการตรวจสอบด้วยชุดทดสอบการย้ายรุ่นแบบจำลองและการคอมไพล์ `ISCC` ขณะที่การรันอัปเกรดจริงในระบบ CI แบบ clean-room ทดสอบเส้นทาง `v0.1.0 -> v0.1.2`
  - ข้อจำกัดด้านเครื่องพิมพ์จริงและกรณี `failed-network-first` จากรุ่น v0.1.1 ยังคงเดิม (ไม่ได้ทดสอบซ้ำกับเครื่องพิมพ์จริง, `failed-network-first` ยังไม่ได้ทดสอบ, และไม่มีการอ้างว่าอาการค้างของกล่องโต้ตอบพิมพ์ทั้งหมดได้รับการแก้ไขแล้ว)
  - ไฟล์ไบนารีของโปรเจกต์ (`pikaview-0.1.2-win-x64-installer.exe`, `ozviewer.exe`, `heif.dll` และ `libde265.dll`) ยังไม่ได้ลงลายเซ็น (`NotSigned`)

## สัญญาอนุญาตและซอร์สโค้ดที่สอดคล้องกัน

ซอร์สโค้ดส่วนแรกอยู่ภายใต้สัญญาอนุญาต MIT (Copyright (c) 2026 OZ Lab) ดูรายละเอียดในไฟล์ `LICENSE` ส่วนประกอบบุคคลที่สามยังคงอยู่ภายใต้สัญญาอนุญาตต้นทางตามที่ระบุใน `THIRD-PARTY-NOTICES.md` (รวมถึง Lucide Icons 1.71.0 ภายใต้ ISC และไอคอนที่สืบเนื่องจาก Feather ภายใต้ MIT), `THIRD-PARTY-LICENSES.txt` และ `THIRD-PARTY-SOURCE.txt`

ส่วนประกอบ LGPL ที่โหลดแบบไดนามิกยังคงเป็น `libheif` 1.23.2 และ `libde265` 1.1.1 (`LGPL-3.0-only`) ซอร์สโค้ดที่สอดคล้องกันทั้งหมด แพตช์ สคริปต์สร้าง และคำแนะนำการสร้างใหม่ ถูกรวบรวมไว้ใน `pikaview-0.1.2-lgpl-corresponding-source.zip` ตามเงื่อนไข LGPLv3 §4 และ GPLv3 §6(d) ผู้ใช้สามารถสร้างใหม่และแทนที่ `heif.dll` และ `libde265.dll` ไว้เคียงข้าง `ozviewer.exe` ได้เนื่องจากแอปโหลดไฟล์เหล่านี้แบบไดนามิก

## ความต่อเนื่องของข้อมูลเชิงประวัติศาสตร์ v0.1.0 และ v0.1.1

OZ Image Viewer 0.1.0 และ PikaView 0.1.1 รวมถึงไฟล์รุ่นที่เผยแพร่และแท็ก git ยังคงเป็นข้อมูลเชิงประวัติศาสตร์ ไม่ถูกเปลี่ยนเลขรุ่นหรือแทนที่ด้วยรุ่น v0.1.2 บันทึก provenance และหลักฐานการสร้างซ้ำได้ถูกเก็บรักษาไว้ที่:
- `docs/releases/v0.1.2/BUILD-PROVENANCE.json`
- `docs/releases/v0.1.2/REPRODUCIBLE-BUILD-PROOF.json`
- `docs/releases/v0.1.1/RELEASE_NOTES.md` (`docs/releases/v0.1.1/BUILD-PROVENANCE.json`)
- `docs/releases/v0.1.0/BUILD-PROVENANCE.json`
