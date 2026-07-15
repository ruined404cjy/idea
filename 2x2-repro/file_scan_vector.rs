//! file:// 向量表供数器：与 file_scan_roundtrip 同构，但 schema 对齐 iceberg_fdw 的
//! vectorsearch_iceberg 用例——c_integer int(required, id=1) + c_vector list<float>(optional,
//! id=2, element-id=3)。用 SDK 在本地 Fs 仓库建表、写真实 Parquet 并 fast_append 提交快照，
//! 打印 metadata_location，供 FDW 用例把 catalog 的 tables_internal.metadata_location 改指过来，
//! 再经 enable_vectorsearch 的本地 TopK 扫出真实近邻行。
//!
//!   cargo test --test file_scan_vector file_scan_vector -- --nocapture
//!
//! 环境变量（默认与 vectorsearch_iceberg 用例约定一致）：
//!   ICEBERG_FILE_SCAN_WAREHOUSE  默认 file:///tmp/iceberg_warehouse
//!   ICEBERG_FILE_SCAN_NAMESPACE  默认 vscan_ns
//!   ICEBERG_FILE_SCAN_TABLE      默认 scan_table

use std::collections::HashMap;
use std::sync::Arc;

use arrow_array::builder::{Float32Builder, ListBuilder};
use arrow_array::{ArrayRef, Int32Array, RecordBatch};
use arrow_cast::cast;
use futures::TryStreamExt;
use iceberg::arrow::schema_to_arrow_schema;
use iceberg::memory::MemoryCatalogBuilder;
use iceberg::spec::{DataFileFormat, ListType, NestedField, PrimitiveType, Schema, Type};
use iceberg::transaction::{ApplyTransactionAction, Transaction};
use iceberg::writer::base_writer::data_file_writer::DataFileWriterBuilder;
use iceberg::writer::file_writer::ParquetWriterBuilder;
use iceberg::writer::file_writer::location_generator::{
    DefaultFileNameGenerator, DefaultLocationGenerator,
};
use iceberg::writer::file_writer::rolling_writer::RollingFileWriterBuilder;
use iceberg::writer::{IcebergWriter, IcebergWriterBuilder};
use iceberg::{Catalog, CatalogBuilder, NamespaceIdent, TableCreation, TableIdent};
use iceberg_storage_opendal::OpenDalStorageFactory;
use parquet::file::properties::WriterProperties;

fn env_or(key: &str, default: &str) -> String {
    std::env::var(key).unwrap_or_else(|_| default.to_string())
}

/// 物理 fixture：query=[1,0,0] 时各行 L2 距离为 0/1/2/3/4，故 LIMIT 3 近邻恰为 id 1,2,3。
const C_INTEGERS: [i32; 5] = [1, 2, 3, 4, 5];
const C_VECTORS: [[f32; 3]; 5] = [
    [1.0, 0.0, 0.0], // d([1,0,0]) = 0
    [2.0, 0.0, 0.0], // d = 1
    [1.0, 2.0, 0.0], // d = 2
    [1.0, 0.0, 3.0], // d = 3
    [5.0, 0.0, 0.0], // d = 4
];

#[test]
fn file_scan_vector() {
    let rt = tokio::runtime::Builder::new_current_thread()
        .enable_all()
        .build()
        .expect("tokio runtime");

    let warehouse = env_or("ICEBERG_FILE_SCAN_WAREHOUSE", "file:///tmp/iceberg_warehouse");
    let ns_name = env_or("ICEBERG_FILE_SCAN_NAMESPACE", "vscan_ns");
    let table_name = env_or("ICEBERG_FILE_SCAN_TABLE", "scan_table");
    let table_location = format!("{warehouse}/{ns_name}/{table_name}");

    // 清掉旧 fixture，保证可重复执行（仅删本测试自己的表目录）。
    let fs_dir = table_location.trim_start_matches("file://").to_string();
    let _ = std::fs::remove_dir_all(&fs_dir);

    let metadata_location = rt.block_on(async {
        let factory: Arc<dyn iceberg::io::StorageFactory> = Arc::new(OpenDalStorageFactory::Fs);
        let mut props = HashMap::new();
        props.insert("warehouse".to_string(), warehouse.clone());
        let catalog = MemoryCatalogBuilder::default()
            .with_storage_factory(factory)
            .load("file_scan_vector", props)
            .await
            .expect("open memory catalog over fs");

        let ns = NamespaceIdent::new(ns_name.clone());
        if !catalog.namespace_exists(&ns).await.expect("namespace_exists") {
            catalog
                .create_namespace(&ns, HashMap::new())
                .await
                .expect("create_namespace");
        }
        let ident = TableIdent::new(ns.clone(), table_name.clone());
        if catalog.table_exists(&ident).await.expect("table_exists") {
            catalog.drop_table(&ident).await.expect("drop_table");
        }

        // 与 vectorsearch_iceberg 用例同构：c_integer int(required, id=1)、
        // c_vector list<float>(optional, id=2, element-id=3, element-required)。
        let schema = Schema::builder()
            .with_schema_id(0)
            .with_fields(vec![
                NestedField::required(1, "c_integer", Type::Primitive(PrimitiveType::Int)).into(),
                NestedField::optional(
                    2,
                    "c_vector",
                    Type::List(ListType::new(
                        NestedField::list_element(3, Type::Primitive(PrimitiveType::Float), true)
                            .into(),
                    )),
                )
                .into(),
            ])
            .build()
            .expect("schema");

        let table = catalog
            .create_table(
                &ns,
                TableCreation::builder()
                    .name(table_name.clone())
                    .schema(schema)
                    .location(table_location.clone())
                    .build(),
            )
            .await
            .expect("create_table");

        let arrow_schema =
            Arc::new(schema_to_arrow_schema(table.metadata().current_schema()).unwrap());
        let c_integer = Int32Array::from(C_INTEGERS.to_vec());
        let mut lb = ListBuilder::new(Float32Builder::new());
        for v in C_VECTORS {
            lb.values().append_slice(&v);
            lb.append(true);
        }
        let c_vector_type = arrow_schema
            .field_with_name("c_vector")
            .unwrap()
            .data_type()
            .clone();
        let c_vector = cast(&(Arc::new(lb.finish()) as ArrayRef), &c_vector_type).unwrap();
        let batch = RecordBatch::try_new(
            arrow_schema,
            vec![Arc::new(c_integer) as ArrayRef, c_vector],
        )
        .expect("record batch");

        let parquet = ParquetWriterBuilder::new(
            WriterProperties::builder().build(),
            table.metadata().current_schema().clone(),
        );
        let rolling = RollingFileWriterBuilder::new_with_default_file_size(
            parquet,
            table.file_io().clone(),
            DefaultLocationGenerator::new(table.metadata().clone()).unwrap(),
            DefaultFileNameGenerator::new("data".to_string(), None, DataFileFormat::Parquet),
        );
        let mut writer = DataFileWriterBuilder::new(rolling)
            .build(None)
            .await
            .expect("data file writer");
        writer.write(batch).await.expect("write batch");
        let data_files = writer.close().await.expect("close writer");

        let tx = Transaction::new(&table);
        let table = tx
            .fast_append()
            .add_data_files(data_files)
            .apply(tx)
            .expect("fast_append")
            .commit(&catalog)
            .await
            .expect("commit");

        // 回读校验：行数应与写入一致。
        let scan = table.scan().select_all().build().expect("build scan");
        let batches: Vec<RecordBatch> = scan
            .to_arrow()
            .await
            .expect("to_arrow")
            .try_collect()
            .await
            .expect("collect batches");
        let rows: usize = batches.iter().map(|b| b.num_rows()).sum();
        assert_eq!(rows, C_INTEGERS.len(), "vector roundtrip row count mismatch");
        assert!(
            table.metadata().current_snapshot().is_some(),
            "committed table must carry a snapshot"
        );

        table
            .metadata_location()
            .expect("metadata_location after commit")
            .to_string()
    });

    println!("METADATA_LOCATION={metadata_location}");
    println!("TABLE_LOCATION={table_location}");
    println!("ROWS={}", C_INTEGERS.len());
}
