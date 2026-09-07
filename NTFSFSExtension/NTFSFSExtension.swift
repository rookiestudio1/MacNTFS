//
//  NTFSFSExtension.swift
//  NTFSFSExtension
//
//  FSKit 擴充的進入點。FSKit 要求進入點必須是 Swift 的 UnaryFileSystemExtension，
//  實際檔案系統邏輯在 Objective-C 的 NTFSFileSystem / NTFSVolume。
//

import ExtensionFoundation
import Foundation
import FSKit

@main
struct NTFSFSExtension: UnaryFileSystemExtension {
  var fileSystem: FSUnaryFileSystem & FSUnaryFileSystemOperations {
    NTFSFileSystem()
  }
}
