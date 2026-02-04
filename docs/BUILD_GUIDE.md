# 빌드 가이드

## 📋 요구 사항

| 항목 | 버전 |
|------|------|
| **OS** | Windows 10 이상 |
| **IDE** | Visual Studio 2022 이상 |
| **C++ 표준** | C++20 |
| **패키지 관리** | vcpkg |

---

## 📦 의존성 설치

### 1. vcpkg 설치

```powershell
# vcpkg 클론
git clone https://github.com/microsoft/vcpkg.git
cd vcpkg

# 부트스트랩
.\bootstrap-vcpkg.bat

# Visual Studio 통합
.\vcpkg integrate install
```

### 2. 패키지 설치

```powershell
# 프로젝트 루트에서 실행 (vcpkg.json 매니페스트 사용)
vcpkg install

# 또는 개별 설치
vcpkg install spdlog:x64-windows
vcpkg install protobuf:x64-windows
vcpkg install grpc:x64-windows
```

### vcpkg.json

```json
{
  "dependencies": [
    "spdlog",
    "protobuf",
    "grpc"
  ]
}
```

---

## 🔨 빌드

### Visual Studio

1. `FastPort.slnx` 솔루션 파일 열기
2. 구성 선택: `Debug` 또는 `Release`
3. 플랫폼 선택: `x64`
4. 빌드: `Ctrl + Shift + B`

### 명령줄 빌드

```powershell
# Developer PowerShell for VS 사용
msbuild FastPort.slnx /p:Configuration=Release /p:Platform=x64
```

---

## 📁 빌드 출력

```
FastPort/
├─ _Output/
│  ├─ x64/
│  │  ├─ Debug/
│  │  │  ├─ FastPortServer.exe
│  │  │  ├─ FastPortClient.exe
│  │  │  └─ *.pdb
│  │  └─ Release/
│  │     ├─ FastPortServer.exe
│  │     └─ FastPortClient.exe
│  └─ ...
└─ _Intermediate/
   └─ ... (중간 파일)
```

---

## ⚙️ 프로젝트 설정

### C++ 언어 표준

모든 프로젝트에서 C++20 사용:

```xml
<PropertyGroup>
  <LanguageStandard>stdcpp20</LanguageStandard>
</PropertyGroup>
```

### C++20 모듈 설정

`.ixx` 파일은 모듈 인터페이스로 컴파일:

```xml
<ClCompile Include="Module.ixx">
  <CompileAs>CompileAsCppModuleInternalPartition</CompileAs>
</ClCompile>
```

### vcpkg 매니페스트 모드

```xml
<PropertyGroup Label="Vcpkg">
  <VcpkgEnableManifest>true</VcpkgEnableManifest>
</PropertyGroup>
```

---

## 🧪 테스트 실행

### Visual Studio Test Explorer

1. `Test` → `Test Explorer` 열기
2. 테스트 빌드: `Build Solution`
3. 테스트 실행: `Run All Tests`

### 명령줄

```powershell
# vstest.console 사용
vstest.console.exe _Output\x64\Debug\LibCommonsTests.dll
vstest.console.exe _Output\x64\Debug\LibNetworksTests.dll
```

---

## 🔧 Protocol Buffers 생성

### .proto 파일 위치

```
Protos/
├─ Commons.proto
└─ Tests.proto
```

### 생성 명령

```powershell
# protoc 실행
protoc --proto_path=Protos --cpp_out=Protocols Protos/*.proto
```

### 생성된 파일

```
Protocols/
├─ Commons.pb.h
├─ Commons.pb.cc
├─ Tests.pb.h
└─ Tests.pb.cc
```

---

## 🚀 실행

### 서버 실행

```powershell
cd _Output\x64\Release
.\FastPortServer.exe
```

### 클라이언트 실행

```powershell
# 별도 터미널에서
cd _Output\x64\Release
.\FastPortClient.exe
```

### 실행 인자 (예정)

```powershell
# 서버
.\FastPortServer.exe --port 9000 --threads 4

# 클라이언트
.\FastPortClient.exe --host 127.0.0.1 --port 9000
```

---

## ❗ 트러블슈팅

### 모듈 빌드 오류

**증상**: `E3496: 모듈 파일을 열 수 없습니다`

**해결**:
1. 솔루션 정리: `Build` → `Clean Solution`
2. `.ifc` 파일 삭제: `_Intermediate` 폴더 삭제
3. 재빌드

### vcpkg 패키지 못 찾음

**증상**: `cannot open include file 'spdlog/spdlog.h'`

**해결**:
```powershell
# vcpkg 통합 확인
vcpkg integrate install

# 패키지 재설치
vcpkg remove spdlog:x64-windows
vcpkg install spdlog:x64-windows
```

### 링커 오류 (protobuf)

**증상**: `unresolved external symbol ... google::protobuf`

**해결**:
- Release/Debug 일치 확인
- `libprotobuf.lib` 링크 확인
- vcpkg triplet 확인 (`x64-windows` vs `x64-windows-static`)
