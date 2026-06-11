# Apache Iceberg Java SDK 数据读取完整指南

本文档详细介绍了 Apache Iceberg Java SDK 的数据读取机制，包括基础 API、JNI 跨语言调用、Arrow 集成以及性能优化策略。

---

## 目录

1. [Iceberg Java SDK 数据读取接口概述](#1-iceberg-java-sdk-数据读取接口概述)
2. [C/C++ 调用 Iceberg Java SDK 的方法](#2-cc-调用-iceberg-java-sdk-的方法)
3. [Iceberg Java SDK 读取流程详解](#3-iceberg-java-sdk-读取流程详解)
4. [planFiles() 方法详解](#4-planfiles-方法详解)
5. [JNI 跨语言调用详解](#5-jni-跨语言调用详解)
6. [Iceberg 与 Arrow 的集成](#6-iceberg 与-arrow-的集成)
7. [iceberg-arrow 模块详解](#7-iceberg-arrow-模块详解)
8. [文件逐个读取机制](#8-文件逐个读取机制)
9. [性能对比：逐行读取 vs 批量读取](#9-性能对比逐行读取-vs-批量读取)
10. [最佳实践与适用场景](#10-最佳实践与适用场景)

---

## 1. Iceberg Java SDK 数据读取接口概述

### 1.1 核心接口

Apache Iceberg Java SDK 提供了完整的数据文件读取接口：

| 接口 | 用途 |
|------|------|
| `Table` | 代表一个 Iceberg 表 |
| `TableScan` | 表扫描操作 |
| `FileScanTask` | 单个文件扫描任务 |
| `DataReader` / `VectorizedReader` | 数据读取器 |
| `Iterable<Record>` | 行迭代器 |

### 1.2 基础读取示例

```java
import org.apache.iceberg.Table;
import org.apache.iceberg.TableScan;
import org.apache.iceberg.DataFile;
import org.apache.iceberg.FileScanTask;
import org.apache.iceberg.catalog.Catalog;
import org.apache.iceberg.hadoop.HadoopTables;
import org.apache.iceberg.expressions.Expressions;
import org.apache.iceberg.io.CloseableIterable;
import org.apache.iceberg.data.Record;
import org.apache.iceberg.data.IcebergGenerics;

// 加载表
HadoopTables tables = new HadoopTables(new Configuration());
Table table = tables.load("/path/to/warehouse/my_table");

// 构建扫描计划
TableScan scan = table.newScan()
    .filter(Expressions.equal("status", "active"))
    .select("id", "name", "status");

// 读取数据
try (CloseableIterable<Record> records = IcebergGenerics.read(table)
        .where(Expressions.equal("status", "active"))
        .select("id", "name")
        .build()) {
    for (Record record : records) {
        Long id = record.get(0, Long.class);
        String name = record.get(1, String.class);
    }
}
```

---

## 2. C/C++ 调用 Iceberg Java SDK 的方法

### 2.1 调用方式概览

有两种主要方式在 C/C++ 中调用 Iceberg Java SDK：

| 方式 | 描述 | 适用场景 |
|------|------|----------|
| JNI | 从 C++ 启动 JVM 并调用 Java 方法 | 简单查询、少量调用 |
| Arrow C Data Interface | 利用 Arrow 的跨语言数据交换能力 | 大数据量传输、生产环境 |
| 服务化 | 通过 gRPC/REST 封装 Iceberg 服务 | 生产环境、解耦架构 |

### 2.2 JNI 基础架构

```
C++ 进程
   │
   │  JNI_CreateJavaVM()
   ▼
JVM 启动
   ├── 加载 Java 类路径
   ├── 初始化垃圾回收器
   └── 创建 JNIEnv 指针
   │
   │  FindClass()
   ▼
类加载
   │
   │  GetStaticMethodID()
   ▼
方法解析
   │
   │  CallStaticObjectMethod()
   ▼
执行 Java 方法
   │
   │  转换返回值
   ▼
C++ 使用结果
```

### 2.3 JNI 示例代码

#### C++ 端

```cpp
#include <jni.h>

class IcebergJNIReader {
public:
    bool initialize(const std::string& classpath) {
        JavaVMInitArgs vm_args;
        JavaVMOption options[4];
        
        // 设置 classpath（包含 Iceberg JAR 包）
        std::string cpOption = "-Djava.class.path=" + classpath;
        options[0].optionString = cpOption.c_str();
        options[1].optionString = "-Xmx4G";
        options[2].optionString = "-Xms1G";
        
        vm_args.version = JNI_VERSION_11;
        vm_args.nOptions = 3;
        vm_args.options = options;
        
        // 创建 JVM
        jint res = JNI_CreateJavaVM(&jvm, (void**)&env, &vm_args);
        return res == JNI_OK;
    }
    
    std::vector<std::string> readTable(
        const std::string& tablePath,
        const std::string& filterColumn,
        const std::string& filterValue
    ) {
        // 加载 Java 类
        jclass readerClass = env->FindClass("com/example/IcebergReader");
        
        // 获取方法 ID
        jmethodID methodId = env->GetStaticMethodID(
            readerClass, "readTable",
            "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)[Ljava/lang/String;"
        );
        
        // 创建参数
        jstring jTablePath = env->NewStringUTF(tablePath.c_str());
        jstring jFilterColumn = env->NewStringUTF(filterColumn.c_str());
        jstring jFilterValue = env->NewStringUTF(filterValue.c_str());
        
        // 调用方法
        jobjectArray jResult = (jobjectArray)env->CallStaticObjectMethod(
            readerClass, methodId,
            jTablePath, jFilterColumn, jFilterValue
        );
        
        // 转换结果
        std::vector<std::string> results;
        jsize length = env->GetArrayLength(jResult);
        for (jsize i = 0; i < length; i++) {
            jstring jStr = (jstring)env->GetObjectArrayElement(jResult, i);
            const char* cStr = env->GetStringUTFChars(jStr, nullptr);
            results.push_back(std::string(cStr));
            env->ReleaseStringUTFChars(jStr, cStr);
            env->DeleteLocalRef(jStr);
        }
        
        return results;
    }
    
private:
    JavaVM* jvm;
    JNIEnv* env;
};
```

#### Java 端

```java
package com.example;

import org.apache.iceberg.Table;
import org.apache.iceberg.hadoop.HadoopTables;
import org.apache.iceberg.expressions.Expressions;
import org.apache.iceberg.data.IcebergGenerics;
import org.apache.iceberg.data.Record;
import org.apache.iceberg.io.CloseableIterable;

public class IcebergReader {
    public static String[] readTable(
            String tablePath, 
            String filterColumn, 
            String filterValue) {
        
        java.util.List<String> results = new java.util.ArrayList<>();
        
        try {
            HadoopTables tables = new HadoopTables();
            Table table = tables.load(tablePath);
            
            CloseableIterable<Record> records = IcebergGenerics.read(table)
                .where(Expressions.equal(filterColumn, filterValue))
                .build();
            
            for (Record record : records) {
                java.util.Map<String, Object> row = new java.util.LinkedHashMap<>();
                for (int i = 0; i < record.size(); i++) {
                    String fieldName = record.struct().fields().get(i).name();
                    row.put(fieldName, record.get(i));
                }
                results.add(new Gson().toJson(row));
            }
            records.close();
        } catch (Exception e) {
            return new String[]{"ERROR: " + e.getMessage()};
        }
        
        return results.toArray(new String[0]);
    }
}
```

### 2.4 JNI 类型映射

| Java 类型 | JNI 类型 | C++ 类型 |
|-----------|----------|----------|
| boolean | jboolean | uint8_t |
| byte | jbyte | int8_t |
| char | jchar | uint16_t |
| short | jshort | int16_t |
| int | jint | int32_t |
| long | jlong | int64_t |
| float | jfloat | float |
| double | jdouble | double |
| String | jstring | 需转换 |
| Object | jobject | void* |
| Object[] | jobjectArray | void* |

---

## 3. Iceberg Java SDK 读取流程详解

### 3.1 Iceberg 表的元数据层次结构

```
Snapshot (快照)
├── snapshot_id: 123456789
├── timestamp_ms: 1704067200000
└── manifest_list: /metadata/snap-123456789-1.avro
    │
    ▼
Manifest List (清单列表)
├── ManifestFile 1: /metadata/manifest-1.avro
├── ManifestFile 2: /metadata/manifest-2.avro
├── ManifestFile 3: /metadata/manifest-3.avro
    │
    ▼
Manifest (清单文件)
├── DataFile 1: /data/part-001.parquet
│   ├── path: /data/part-001.parquet
│   ├── format: PARQUET
│   ├── partition: {year=2024, month=01}
│   ├── record_count: 100000
│   ├── file_size: 1048576
│   ├── column_sizes: {col1=1024, col2=2048}
│   ├── lower_bounds: {col1=[min_value]}
│   └── upper_bounds: {col1=[max_value]}
├── DataFile 2: /data/part-002.parquet
└── DataFile 3: /data/part-003.parquet
```

### 3.2 读取流程五个步骤

```java
// 第1步：初始化 Catalog 和加载表
HadoopTables tables = new HadoopTables(new Configuration());
Table table = tables.load("/path/to/warehouse/my_table");

// 第2步：构建表扫描计划
TableScan scan = table.newScan()
    .filter(Expressions.equal("status", "active"))
    .filter(Expressions.greaterThan("created_at", "2024-01-01"))
    .select("id", "name", "status", "created_at");

// 第3步：执行扫描计划（获取文件列表）
try (CloseableIterable<FileScanTask> tasks = scan.planFiles()) {
    for (FileScanTask task : tasks) {
        // 第4步：获取文件元信息
        DataFile dataFile = task.file();
        System.out.println("文件路径: " + dataFile.path());
        System.out.println("文件格式: " + dataFile.format());
        System.out.println("记录数: " + dataFile.recordCount());
    }
}

// 第5步：读取实际数据
try (CloseableIterable<Record> records = IcebergGenerics.read(table)
        .where(Expressions.equal("status", "active"))
        .select("id", "name")
        .build()) {
    for (Record record : records) {
        Long id = record.get(0, Long.class);
        String name = record.get(1, String.class);
    }
}
```

---

## 4. planFiles() 方法详解

### 4.1 核心概念

**`planFiles()` 只返回元数据，不读取实际数据**

```java
CloseableIterable<FileScanTask> tasks = scan.planFiles();
```

这行代码生成扫描计划，返回需要扫描的文件列表，但不读取任何实际数据内容。

### 4.2 FileScanTask 接口

```java
public interface FileScanTask extends ScanTask {
    // 文件信息（元数据，不含实际数据）
    DataFile file();
    
    // 扫描范围信息
    long start();      // 文件中的起始偏移量
    long length();     // 扫描的长度（字节）
    
    // 分区信息
    PartitionSpec spec();
    
    // 删除文件（用于行级删除）
    List<DeleteFile> deletes();
}
```

### 4.3 DataFile 接口

```java
public interface DataFile {
    // 文件基础信息
    String path();              // 文件路径
    FileFormat format();        // 文件格式: PARQUET, ORC, AVRO
    
    // 分区信息
    StructLike partition();     // 分区值
    
    // 统计信息（用于谓词下推）
    long recordCount();         // 记录数
    long fileSizeInBytes();     // 文件大小
    Map<Integer, Long> columnSizes();        // 各列大小
    Map<Integer, Long> valueCounts();        // 各列值数量
    Map<Integer, Long> nullValueCounts();    // 各列 NULL 数量
    Map<Integer, ByteBuffer> lowerBounds();  // 各列最小值
    Map<Integer, ByteBuffer> upperBounds();  // 各列最大值
}
```

### 4.4 planFiles() 与实际数据读取的区别

| 操作 | 读取的文件 | 返回的内容 | 是否有实际数据 |
|------|-----------|-----------|---------------|
| `planFiles()` | manifest 文件（几KB） | FileScanTask 列表 | ❌ 无 |
| `IcebergGenerics.read()` | Parquet 文件（几GB） | Record 对象 | ✅ 有 |
| `ArrowReader.read()` | Parquet 文件（几GB） | Arrow RecordBatch | ✅ 有 |

### 4.5 两阶段设计的好处

**场景**：查询 `SELECT * FROM users WHERE year=2024 AND id > 1000000`

| 阶段 | 作用 | I/O 开销 |
|------|------|----------|
| 阶段1: planFiles() | 利用分区过滤和统计信息过滤文件 | 几KB |
| 阶段2: 读取实际数据 | 只读取阶段1确定的文件 | 几GB |

**举例**：
- 表有 1000 个 Parquet 文件，每个 1GB
- 无过滤：需要扫描 1000GB
- 有分区过滤（year=2024）：只扫描 12 个文件 = 12GB
- 有统计信息过滤（id > 1000000）：进一步减少到 3 个文件 = 3GB
- **性能提升：1000GB → 3GB，减少 99.7%**

---

## 5. JNI 跨语言调用详解

### 5.1 JNI 调用完整流程

```
C++ 进程
   │
   │  1. JNI_CreateJavaVM()
   ▼
JVM 启动
   ├── 加载 Java 类路径
   ├── 初始化垃圾回收器
   └── 创建 JNIEnv 指针
   │
   │  2. FindClass("com/example/IcebergReader")
   ▼
类加载
   ├── 查找 .class 文件
   ├── 加载 IcebergReader 类
   └── 返回 jclass 引用
   │
   │  3. GetStaticMethodID()
   ▼
方法解析
   ├── 验证方法签名
   ├── 获取方法 ID
   └── 缓存方法 ID
   │
   │  4. NewStringUTF() 创建参数
   ▼
参数准备
   C++ std::string → Java jstring
   │
   │  5. CallStaticObjectMethod()
   ▼
进入 Java 世界
   ├── IcebergReader.readTable()
   ├── 加载 Iceberg 表
   ├── 执行扫描计划
   ├── 读取数据文件
   └── 返回 String[]
   │
   │  6. 转换返回值
   ▼
结果提取
   jobjectArray → C++ std::vector<std::string>
   │
   │  7. DestroyJavaVM()
   ▼
JVM 销毁
```

### 5.2 JNI 的挑战

| 挑战 | 描述 | 解决方案 |
|------|------|----------|
| JVM 生命周期管理 | 整个进程只能创建/销毁一次 JVM | 单例模式，进程启动时创建 |
| 错误处理复杂 | JNI 异常需要手动检查和清理 | 封装 JNI 调用，统一异常处理 |
| 数据传递开销 | 序列化/反序列化有性能损耗 | 使用 Arrow 零拷贝传递 |
| 多线程处理 | JNIEnv 不能跨线程共享 | 每线程获取自己的 JNIEnv |

---

## 6. Iceberg 与 Arrow 的集成

### 6.1 什么是 Arrow？

**Apache Arrow** 是一种跨语言的内存列式数据格式：

```
传统数据交换方式：
Python (Pandas) → JSON → Java (Spark) → JSON → C++ (计算引擎)
问题：每次传递都要序列化/反序列化，开销巨大！

Arrow 零拷贝交换：
Python → Arrow 内存格式（共享内存） ← Java ← C++
优势：零拷贝，性能提升 10-100 倍！
```

### 6.2 Iceberg 如何支持 Arrow？

Iceberg 通过 `iceberg-arrow` 模块实现与 Arrow 的集成：

```java
import org.apache.iceberg.arrow.VectorizedTableScanIterable;
import org.apache.arrow.memory.RootAllocator;
import org.apache.arrow.vector.VectorSchemaRoot;

Table table = tables.load("/path/to/table");
RootAllocator allocator = new RootAllocator(Long.MAX_VALUE);

try (VectorizedTableScanIterable iterator = new VectorizedTableScanIterable(
        table.newScan().select("id", "name", "value"),
        allocator,
        4096  // 批次大小
)) {
    for (VectorSchemaRoot batch : iterator) {
        Int64Vector idVector = (Int64Vector) batch.getVector("id");
        VarCharVector nameVector = (VarCharVector) batch.getVector("name");
        
        for (int i = 0; i < batch.getRowCount(); i++) {
            long id = idVector.get(i);
            String name = new String(nameVector.get(i));
        }
    }
}
```

### 6.3 Arrow 与 C++ 集成

通过 Arrow C Data Interface 实现 Java 到 C++ 的零拷贝传递：

```
Iceberg 表 (Parquet 文件)
    │
    │ ArrowReader 读取并转换
    ▼
Arrow RecordBatch（列式内存格式）
    │
    │ C Data Interface（传递指针）
    ▼
C++ Arrow 库
    ├── 直接使用同一块内存
    └── 无需拷贝
```

---

## 7. iceberg-arrow 模块详解

### 7.1 模块依赖关系

```
用户代码
    │
    ▼
iceberg-arrow 模块
├── VectorizedTableScanIterable
├── ArrowBatchReader
├── ParquetToArrowConverter
│   作用：将 Iceberg 数据转换为 Arrow 格式
    │
    ▼
iceberg-core 模块（Iceberg SDK 核心）
├── Table / TableScan
├── FileScanTask / DataFile
├── planFiles()
│   作用：管理 Iceberg 表、获取文件列表
    │
    ▼
iceberg-parquet 模块
├── ParquetReader
│   作用：读取 Parquet 文件的原始数据
    │
    ▼
Arrow SDK (apache-arrow-java)
├── BufferAllocator
├── VectorSchemaRoot
├── Int64Vector / VarCharVector
│   作用：提供 Arrow 内存格式和数据结构
```

### 7.2 各模块职责分工

| 模块 | 负责内容 |
|------|----------|
| iceberg-core | 表管理和元数据、planFiles()、分区过滤、谓词下推 |
| iceberg-parquet | Parquet 文件读取、解码压缩数据 |
| iceberg-arrow | 格式转换（Parquet → Arrow）、批量读取 |
| Arrow SDK | Arrow 内存格式、VectorSchemaRoot |

### 7.3 内部执行流程

```
第1阶段：Iceberg SDK 处理
─────────────────────────
CloseableIterable<FileScanTask> tasks = scan.planFiles();
返回：FileScanTask 1, 2, 3...

第2阶段：逐文件处理
─────────────────────────
for (FileScanTask task : tasks) {
    // 2.1 打开 Parquet 文件
    ParquetFileReader reader = ParquetFileReader.open(task.file().path());
    
    // 2.2 读取数据块
    while (reader.hasNextRowGroup()) {
        PageReadStore pages = reader.readNextRowGroup();
        
        // 2.3 转换为 Arrow 格式
        VectorSchemaRoot batch = ParquetToArrowConverter.convert(
            pages, allocator, batchSize=4096
        );
        
        // 2.4 返回给用户
        yield batch;
    }
}
```

---

## 8. 文件逐个读取机制

### 8.1 两种读取方式的代码对比

#### 方式 A：逐行读取

```java
try (CloseableIterable<Record> records = IcebergGenerics.read(table)
        .where(Expressions.equal("status", "active"))
        .build()) {
    
    for (Record record : records) {
        // 每次迭代返回一行数据
        // 内部自动处理文件切换
        Long id = record.get(0, Long.class);
        String name = record.get(1, String.class);
    }
}

// 执行顺序：
// 1. 打开 part-001.parquet
// 2. 返回 Record 1 → 用户处理
// 3. 返回 Record 2 → 用户处理
// ...
// 4. part-001.parquet 读完，关闭文件
// 5. 打开 part-002.parquet
// ...
```

#### 方式 B：逐文件读取（手动控制）

```java
try (CloseableIterable<FileScanTask> tasks = scan.planFiles()) {
    for (FileScanTask task : tasks) {
        DataFile dataFile = task.file();
        
        try (CloseableIterable<Record> fileRecords = new DataFileReader(table, task)) {
            for (Record record : fileRecords) {
                // 处理这个文件内的每一行
            }
        }
    }
}
```

### 8.2 为什么设计成逐文件读取？

| 原因 | 描述 |
|------|------|
| 资源管理 | 同时只打开 1 个文件，避免内存溢出和文件句柄超限 |
| 支持并行处理 | FileScanTask 列表可分配给多个线程并行处理 |
| 支持增量处理 | 用户可在迭代中途停止，节省 I/O |

---

## 9. 性能对比：逐行读取 vs 批量读取

### 9.1 核心区别

| 特性 | 逐行读取 | 批量读取 |
|------|--------------------------|----------------------------|
| 返回单位 | 每次返回 1 行 Record | 每次返回一批（4096行）VectorSchemaRoot |
| 内存格式 | Java 对象（堆内存） | Arrow 列向量（直接内存） |
| Java 对象数 | 每行创建多个对象 | 0 个对象 |
| GC 压力 | 高 | 极低 |
| CPU 缓存 | 不友好（跳跃访问） | 友好（连续访问） |

### 9.2 内存布局对比

#### 逐行读取的内存布局（堆内存）

```
Record 1                      Record 2
┌─────────────────────┐      ┌─────────────────────┐
│ id: Long对象         │      │ id: Long对象         │
│   ├─ header: 16B    │      │   ├─ header: 16B    │
│   ├─ value: 8B      │      │   ├─ value: 8B      │
│                     │      │                     │
│ name: String对象     │      │ name: String对象     │
│   ├─ header: 24B    │      │   ├─ header: 24B    │
│   ├─ "Alice"        │      │   ├─ "Bob"          │
│                     │      │                     │
│ value: Double对象    │      │ value: Double对象    │
│   ├─ header: 16B    │      │   ├─ header: 16B    │
│   ├─ 1.1            │      │   ├─ 2.2            │
└─────────────────────┘      └─────────────────────┘

内存地址：分散在堆内存各处
问题：对象头开销大（每个值 16-24 字节 header）
      CPU 缓存不友好（跳跃访问）
```

#### 批量读取的内存布局

```
Int64Vector "id"（连续内存）
┌─────────────────────────────────────────────────────────────────────┐
│ [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, ...]                                │
│ 内存地址: 0x1000 → 0x1008 → 0x1010 → 0x1018 → ...                   │
│ 连续 8 字节间隔，CPU 缓存友好                                         │
└─────────────────────────────────────────────────────────────────────┘

VarCharVector "name"（连续内存）
┌─────────────────────────────────────────────────────────────────────┐
│ offsets: [0, 5, 9, 14, 19, ...]                                     │
│ data: "AliceBobCarolDaveEve..."                                     │
└─────────────────────────────────────────────────────────────────────┘

Float8Vector "value"（连续内存）
┌─────────────────────────────────────────────────────────────────────┐
│ [1.1, 2.2, 3.3, 4.4, 5.5, ...]                                      │
└─────────────────────────────────────────────────────────────────────┘

优势：
├── 无对象 header 开销
├── CPU 缓存友好（连续访问）
└── 可利用 SIMD 指令批量处理
```

### 9.3 CPU 缓存效率对比

| 方式 | 缓存命中率 | 原因 |
|------|-----------|------|
| 逐行读取 | ~40% | 每行处理跳跃访问不同内存地址 |
| 批量读取 | ~90% | 1 次缓存加载可处理 8 个值 |

### 9.4 内存拷贝次数对比

#### 方式 1：Record → Arrow 的内存拷贝

```
步骤                     拷贝次数
─────────────────────────────────
1. Parquet 压缩数据        0
2. Parquet 解码缓冲区      1次
3. Record 对象            2次（创建 Long/String 对象）
4. Arrow Vector           3次（从 Record 拷贝）

总拷贝次数：每个值被拷贝 3 次
```

#### 方式 2：iceberg-arrow 直接转换

```
步骤                     拷贝次数
─────────────────────────────────
1. Parquet 压缩数据        0
2. Arrow Vector           1次（直接解码写入）

总拷贝次数：每个值被拷贝 1 次
节省：66% 内存拷贝
```

### 9.5 Java 对象开销详解

假设读取 1 亿行数据，每行有 1 个 Long 字段：

| 方式 | 内存占用 | 有效数据占比 | GC 开销 |
|------|----------|-------------|--------|
| Java Long 对象 | 2.4GB | 33%（8B/24B） | 高（扫描 1 亿对象） |
| Arrow Int64Vector | 800MB | 99% | 无 |

### 9.6 性能数据对比（1亿行，3列）

| 指标 | 逐行读取 | 批量读取 | 提升 |
|------|----------|----------|------|
| Java 对象数 | 3亿个 | 0 | - |
| 堆内存占用 | ~8GB | ~50MB | 99% |
| GC 时间 | ~5秒 | 0 | - |
| 缓存命中率 | ~40% | ~90% | 2.25x |
| 求和聚合耗时 | 25秒 | 3秒 | 8x |
| 简单读取耗时 | 20秒 | 2秒 | 10x |

### 9.7 iceberg-arrow 的核心优势总结

```
优势 1：跳过中间层
─────────────────────────
方式1：Parquet → Record → Arrow
方式2：Parquet → Arrow（直接）
跳过 Record 对象意味着：
├── 无 Java 对象创建
├── 无对象头开销
├── 无 GC 扫描
└── 减少一次完整的数据拷贝

优势 2：批量操作
─────────────────────────
方式1：逐行处理（每次 1 行）
方式2：批量处理（每次 4096 行）
├── 批量解码（一次解码整块）
├── 批量拷贝（一次拷贝整块）
└── CPU 缓存友好，可利用 SIMD

优势 3：内存效率
─────────────────────────
方式1：堆内存 + 直接内存（需拷贝）
方式2：直接内存（解码直接写入）
├── 无堆内存参与
└── 可零拷贝传给 C++

优势 4：向量化读取
─────────────────────────
├── 利用 Parquet 的向量化解码 API
├── 批量解码列数据
└── 与 Arrow 列式内存天然匹配
```

---

## 10. 最佳实践与适用场景

### 10.1 适用场景对比

#### 逐行读取适合的场景

| 场景 | 描述 |
|------|------|
| 简单的业务逻辑 | 每行独立处理、复杂的行级判断 |
| 数据量较小 | < 10万行，性能差异不明显 |
| 需要访问所有列 | 列式优势不明显 |
| 复杂的行级逻辑 | 多列之间的复杂计算 |

#### 批量读取适合的场景

| 场景 | 描述 |
|------|------|
| 聚合计算 | SUM, COUNT, AVG, MAX, MIN, GROUP BY |
| 大数据量分析 | > 100万行，性能优势明显 |
| 只访问少数列 | 列裁剪优势明显 |
| 向量化计算 | SIMD 加速、批量处理 |
| 与其他系统集成 | Spark, Flink, Pandas, DuckDB |
| 跨语言传递 | Java → C++ 零拷贝 |

### 10.2 调用方式选择

| 场景 | 推荐方案 |
|------|----------|
| 简单查询、少量调用 | JNI 直接调用 |
| 大数据量传输 | Arrow + C Data Interface |
| 生产环境 | 服务化（gRPC/REST） |

### 10.3 最佳实践建议

```
1. 根据数据量选择读取方式
   ├── < 10万行：逐行读取即可
   ├── 10万-100万行：视场景选择
   └── > 100万行：优先使用批量读取

2. 根据操作类型选择
   ├── 行级业务逻辑：逐行读取
   ├── 聚合分析：批量读取
   └── 跨语言传递：Arrow + C Data Interface

3. 根据集成需求选择
   ├── 简单集成：JNI
   ├── 高性能集成：Arrow
   └── 生产级集成：服务化

4. 资源管理注意事项
   ├── 逐行读取：注意 GC 调优
   ├── 批量读取：注意直接内存限制
   ├── JNI：注意 JVM 生命周期管理
   └── Arrow：注意内存分配器配置
```

---

## 附录：关键概念总结

### A. Iceberg 元数据层次

```
Table → Snapshot → ManifestList → Manifest → DataFile
```

### B. planFiles() 核心作用

```
planFiles() = 读取元数据，确定文件列表
实际数据读取 = 读取 Parquet/ORC 文件内容
```

### C. iceberg-arrow 价值

```
iceberg-arrow 优势 = 批量列式处理 + 无 Java 对象 + 零拷贝传递
```

### D. JNI 关键步骤

```
1. JNI_CreateJavaVM()  → 创建 JVM
2. FindClass()         → 加载类
3. GetStaticMethodID() → 获取方法
4. CallStaticObjectMethod() → 调用方法
5. 转换返回值          → C++ 使用结果
6. DestroyJavaVM()     → 销毁 JVM
```

---

## 参考资源

- [Apache Iceberg 官方文档](https://iceberg.apache.org/)
- [Apache Arrow 官方文档](https://arrow.apache.org/)
- [Arrow C Data Interface](https://arrow.apache.org/docs/format/CDataInterface.html)
- [JNI 官方规范](https://docs.oracle.com/javase/8/docs/technotes/guides/jni/)
- [Baeldung JNI 教程](https://www.baeldung.com/jni)
- [Iceberg FileScanTask API](https://iceberg.apache.org/docs/latest/api/)