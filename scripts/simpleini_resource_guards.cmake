# 为固定的 SimpleIni 4.19 生成仅供 config 使用的私有头文件。
# 不修改 FetchContent 源，不引入其他版本的解析/序列化或增量回滚行为。
# 上游 MIT 许可证保留在生成头文件中。
function(xen_prepare_simpleini source_header output_directory)
    file(READ "${source_header}" header)
    string(REPLACE "\r\n" "\n" header "${header}")
    string(SHA256 digest "${header}")
    if(NOT digest STREQUAL "91bfe39aa476f20bc27f71c70fe3420197464ef1b16ce4b2b64938610df1241e")
        message(FATAL_ERROR "SimpleIni header differs from pinned 4.19; review resource guards before updating it")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${source_header}")

    string(REPLACE "#include <string>" "#include <string>\n#include <memory>" header "${header}")

    # 两个路径重载拥有 fopen/_wfopen 的文件；FILE* 重载仍只借用调用者的文件。
    string(REPLACE [=[    SI_Error rc = LoadFile(fp);
    fclose(fp);
    return rc;]=] [=[    std::unique_ptr<FILE, decltype(&fclose)> fileOwner(fp, &fclose);
    return LoadFile(fileOwner.get());]=] header "${header}")

    string(REPLACE [=[    pData[lSize] = 0;]=] [=[    std::unique_ptr<char[]> dataOwner(pData);
    pData[lSize] = 0;]=] header "${header}")
    string(REPLACE [=[    if (uRead != (size_t) lSize) {
        delete[] pData;
        return SI_FILE;
    }]=] [=[    if (uRead != (size_t) lSize) {
        return SI_FILE;
    }]=] header "${header}")
    string(REPLACE [=[    SI_Error rc = LoadData(pData, uRead);
    delete[] pData;
    return rc;]=] [=[    return LoadData(pData, uRead);]=] header "${header}")

    # 首次解析失败时，部分表项/注释指向尚未接管的数组；先清表再释放数组。
    # 已有数据的增量加载仍保留 4.19 行为，仅释放本次临时解析数组。
    string(REPLACE [=[    memset(pData, 0, sizeof(SI_CHAR)*(uLen+1));]=] [=[    auto releaseData = [this](SI_CHAR * data) noexcept {
        if (!m_pData) {
            m_data.clear();
            m_pFileComment = NULL;
        }
        delete[] data;
    };
    std::unique_ptr<SI_CHAR[], decltype(releaseData)> dataOwner(pData, releaseData);
    memset(pData, 0, sizeof(SI_CHAR)*(uLen+1));]=] header "${header}")
    string(REPLACE [=[    if (!converter.ConvertFromStore(a_pData, a_uDataLen, pData, uLen)) {
        delete[] pData;
        return SI_FAIL;
    }]=] [=[    if (!converter.ConvertFromStore(a_pData, a_uDataLen, pData, uLen)) {
        return SI_FAIL;
    }]=] header "${header}")
    string(REPLACE [=[    if (bCopyStrings) {
        delete[] pData;
    }
    else {
        m_pData = pData;
        m_uDataLen = uLen+1;
    }]=] [=[    if (!bCopyStrings) {
        m_pData = dataOwner.release();
        m_uDataLen = uLen+1;
    }]=] header "${header}")

    # 保存配置会复制字符串；list 登记抛出时，数组仍由本次调用负责释放。
    string(REPLACE [=[    memcpy(pCopy, a_pString, sizeof(SI_CHAR)*uLen);
    m_strings.push_back(pCopy);
    a_pString = pCopy;]=] [=[    std::unique_ptr<SI_CHAR[]> copyOwner(pCopy);
    memcpy(pCopy, a_pString, sizeof(SI_CHAR)*uLen);
    m_strings.push_back(pCopy);
    a_pString = copyOwner.release();]=] header "${header}")

    file(MAKE_DIRECTORY "${output_directory}")
    # 相同内容不更新 mtime，避免每次配置都重编译 config。
    file(CONFIGURE OUTPUT "${output_directory}/SimpleIni.h" CONTENT "${header}" @ONLY)
endfunction()
