﻿// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License. See LICENSE in the project root for license information.

#include "pch.h"
#include <EnvironmentManager.h>
#include <EnvironmentManager.g.cpp>
#include <EnvironmentVariableChangeTracker.h>
#include <PathChangeTracker.h>
#include <PathExtChangeTracker.h>
#include <IChangeTracker.h>

namespace winrt::Microsoft::ProjectReunion::implementation
{

    EnvironmentManager::EnvironmentManager(Scope const& scope)
        : m_Scope(scope) { }

    ProjectReunion::EnvironmentManager EnvironmentManager::GetForProcess()
    {
        ProjectReunion::EnvironmentManager environmentManager{ nullptr };
        environmentManager = winrt::make<implementation::EnvironmentManager>(Scope::Process);
        return environmentManager;
    }

    Microsoft::ProjectReunion::EnvironmentManager EnvironmentManager::GetForUser()
    {
        ProjectReunion::EnvironmentManager environmentManager{ nullptr };
        environmentManager = winrt::make<implementation::EnvironmentManager>(Scope::User);
        return environmentManager;
    }

    Microsoft::ProjectReunion::EnvironmentManager EnvironmentManager::GetForMachine()
    {
        ProjectReunion::EnvironmentManager environmentManager{ nullptr };
        environmentManager = winrt::make<implementation::EnvironmentManager>(Scope::Machine);
        return environmentManager;
    }

    Windows::Foundation::Collections::IMapView<hstring, hstring> EnvironmentManager::GetEnvironmentVariables()
    {
        Windows::Foundation::Collections::StringMap environmentVariables;

        if (m_Scope == Scope::Process)
        {
            environmentVariables = GetProcessEnvironmentVariables();
        }
        else
        {
            environmentVariables = GetUserOrMachineEnvironmentVariables();
        }

        return environmentVariables.GetView();
    }

    hstring EnvironmentManager::GetEnvironmentVariable(hstring const& variableName)
    {
        if (variableName.empty())
        {
            THROW_HR(E_INVALIDARG);
        }

        if (m_Scope == Scope::Process)
        {
            return hstring{ GetProcessEnvironmentVariable(std::wstring{variableName }) };
        }
        else
        {
            return hstring{ GetUserOrMachineEnvironmentVariable(std::wstring{variableName}) };
        }
    }

    bool EnvironmentManager::IsSupported()
    {
        throw hresult_not_implemented();
    }

    void EnvironmentManager::SetEnvironmentVariable(hstring const& name, hstring const& value)
    {
        auto setEV = [&, name, value, this]()
        {
            if (m_Scope == Scope::Process)
            {
                BOOL result{ FALSE };
                if (!value.empty())
                {
                    result = ::SetEnvironmentVariable(name.c_str(), value.c_str());
                }
                else
                {
                    result = ::SetEnvironmentVariable(name.c_str(), nullptr);
                }

                if (result == TRUE)
                {
                    return S_OK;
                }
                else
                {
                    DWORD lastError{ GetLastError() };
                    RETURN_HR(HRESULT_FROM_WIN32(lastError));
                }
            }

            // m_Scope should be user or machine here.
            wil::unique_hkey environmentVariableKey{ GetRegHKeyForEVUserAndMachineScope(true) };

            if (!value.empty())
            {
                THROW_IF_FAILED(HRESULT_FROM_WIN32(RegSetValueEx(
                    environmentVariableKey.get()
                    , name.c_str()
                    , 0
                    , REG_SZ
                    , reinterpret_cast<const BYTE*>(value.c_str())
                    , static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)))));
            }
            else
            {
                DeleteEnvironmentVariableIfExists(environmentVariableKey.get(), name.c_str());
            }

            const WPARAM c_noWParam{};
            LRESULT broadcastResult { SendMessageTimeout(HWND_BROADCAST, WM_SETTINGCHANGE,
                c_noWParam, reinterpret_cast<LPARAM>(L"Environment"),
                SMTO_NOTIMEOUTIFNOTHUNG | SMTO_BLOCK, 1000, nullptr) };

            if (broadcastResult == 0)
            {
                THROW_HR(HRESULT_FROM_WIN32(GetLastError()));
            }

            return S_OK;
        };

        EnvironmentVariableChangeTracker changeTracker(std::wstring(name), std::wstring(value), m_Scope);

        THROW_IF_FAILED(changeTracker.TrackChange(setEV));
    }

    void EnvironmentManager::AppendToPath(hstring const& path)
    {
        THROW_HR_IF(E_INVALIDARG, path.empty());

        // Get the existing path because we will append to it.
        std::wstring existingPath{ GetPath() };

        // Don't append to the path if the addition already exists.
        if (existingPath.find(path) != std::wstring::npos)
        {
            return;
        }

        std::wstring newPath{ existingPath.append(path) };

        if (newPath.back() != L';')
        {
            newPath += L';';
        }

        auto setPath = [&, newPath, this]()
        {
            if (m_Scope == Scope::Process)
            {
                THROW_IF_WIN32_BOOL_FALSE(::SetEnvironmentVariable(c_PathName, newPath.c_str()));
            }
            else //Scope is either user or machine
            {
                wil::unique_hkey environmentVariableKey{ GetRegHKeyForEVUserAndMachineScope(true) };

                THROW_IF_FAILED(HRESULT_FROM_WIN32(RegSetValueEx(
                    environmentVariableKey.get()
                    , c_PathName
                    , 0
                    , REG_EXPAND_SZ
                    , reinterpret_cast<const BYTE*>(newPath.c_str())
                    , static_cast<DWORD>((newPath.size() + 1) * sizeof(wchar_t)))));

                LRESULT broadcastResult{ SendMessageTimeout(HWND_BROADCAST, WM_SETTINGCHANGE,
                    reinterpret_cast<WPARAM>(nullptr), reinterpret_cast<LPARAM>(L"Environment"),
                    SMTO_NOTIMEOUTIFNOTHUNG | SMTO_BLOCK, 1000, nullptr) };

                if (broadcastResult == 0)
                {
                    THROW_HR(HRESULT_FROM_WIN32(GetLastError()));
                }
            }

            return S_OK;
        };

        PathChangeTracker changeTracker(std::wstring(path), m_Scope, IChangeTracker::PathOperation::Append);

        THROW_IF_FAILED(changeTracker.TrackChange(setPath));
    }

    void EnvironmentManager::RemoveFromPath(hstring const& path)
    {
        THROW_HR_IF(E_INVALIDARG, path.empty());

        // A user is only allowed to remove something from the PATH if
        // 1. path exists in PATH
        // 2. path matches a path part exactly (ignoring case)
        std::wstring currentPath{ GetPath() };
        std::wstring pathPartToFind(path);

        if (pathPartToFind.back() != L';')
        {
            pathPartToFind += L';';
        }

        int left{ static_cast<int>(currentPath.size()) };
        int right{ static_cast<int>(currentPath.size() - 1) };
        bool foundPathPart{ false };

        while (!foundPathPart && left >= 0)
        {
            if (left == 0 || currentPath[left - 1] == L';')
            {
                std::wstring pathPart(currentPath, left, right - left);

                if (CompareStringOrdinal(pathPart.c_str(), -1,
                    pathPartToFind.c_str(), -1,
                    TRUE) == CSTR_EQUAL)
                {
                    foundPathPart = true;
                }
                else
                {
                    right = left;
                }
            }

            left--;
        }

        // foundPathPart is used to check if we need to track and
        // apply the change.
        currentPath.erase(left + 1, right - left - 1);

        auto removeFromPath = [&, currentPath, this]()
        {
            if (m_Scope == Scope::Process)
            {
                THROW_IF_WIN32_BOOL_FALSE(::SetEnvironmentVariable(c_PathName, currentPath.c_str()));
            }
            else //Scope is either user or machine
            {
                wil::unique_hkey environmentVariableKey{ GetRegHKeyForEVUserAndMachineScope(true) };

                THROW_IF_FAILED(HRESULT_FROM_WIN32(RegSetValueEx(
                    environmentVariableKey.get()
                    , c_PathName
                    , 0
                    , REG_EXPAND_SZ
                    , reinterpret_cast<const BYTE*>(currentPath.c_str())
                    , static_cast<DWORD>((currentPath.size() + 1) * sizeof(wchar_t)))));

                LRESULT broadcastResult{ SendMessageTimeout(HWND_BROADCAST, WM_SETTINGCHANGE,
                    reinterpret_cast<WPARAM>(nullptr), reinterpret_cast<LPARAM>(L"Environment"),
                    SMTO_NOTIMEOUTIFNOTHUNG | SMTO_BLOCK, 1000, nullptr) };

                if (broadcastResult == 0)
                {
                    THROW_HR(HRESULT_FROM_WIN32(GetLastError()));
                }
            }

            return S_OK;
        };

        if (foundPathPart)
        {
            PathChangeTracker changeTracker(std::wstring(path), m_Scope, IChangeTracker::PathOperation::Remove);

            THROW_IF_FAILED(changeTracker.TrackChange(removeFromPath));
        }
    }

    void EnvironmentManager::AddExecutableFileExtension(hstring const& pathExt)
    {
        THROW_HR_IF(E_INVALIDARG, pathExt.empty());

        // Get the existing path because we will append to it.
        std::wstring existingPathExt{ GetPathExt() };

        // Don't append to the path if the addition already exists.
        if (existingPathExt.find(pathExt) != std::wstring::npos)
        {
            return;
        }

        std::wstring newPathExt{ existingPathExt.append(pathExt) };

        if (newPathExt.back() != L';')
        {
            newPathExt += L';';
        }

        auto setPathExt = [&, newPathExt, this]()
        {
            if (m_Scope == Scope::Process)
            {
                THROW_IF_WIN32_BOOL_FALSE(::SetEnvironmentVariable(c_PathExtName, newPathExt.c_str()));
            }
            else //Scope is either user or machine
            {
                wil::unique_hkey environmentVariableKey{ GetRegHKeyForEVUserAndMachineScope(true) };

                THROW_IF_FAILED(HRESULT_FROM_WIN32(RegSetValueEx(
                    environmentVariableKey.get()
                    , c_PathExtName
                    , 0
                    , REG_EXPAND_SZ
                    , reinterpret_cast<const BYTE*>(newPathExt.c_str())
                    , static_cast<DWORD>((newPathExt.size() + 1) * sizeof(wchar_t)))));

                LRESULT broadcastResult{ SendMessageTimeout(HWND_BROADCAST, WM_SETTINGCHANGE,
                    reinterpret_cast<WPARAM>(nullptr), reinterpret_cast<LPARAM>(L"Environment"),
                    SMTO_NOTIMEOUTIFNOTHUNG | SMTO_BLOCK, 1000, nullptr) };

                if (broadcastResult == 0)
                {
                    THROW_HR(HRESULT_FROM_WIN32(GetLastError()));
                }
            }

            return S_OK;
        };

        PathExtChangeTracker changeTracker(std::wstring(newPathExt), m_Scope, IChangeTracker::PathOperation::Append);

        THROW_IF_FAILED(changeTracker.TrackChange(setPathExt));
    }

    void EnvironmentManager::RemoveExecutableFileExtension(hstring const& pathExt)
    {
        THROW_HR_IF(E_INVALIDARG, pathExt.empty());

        // A user is only allowed to remove something from the PATHEXT if
        // 1. path exists in PATHEXT
        // 2. path matches a path part exactly (ignoring case)
        std::wstring currentPathExt{ GetPathExt() };
        std::wstring pathExtPartToFind(pathExt);

        if (pathExtPartToFind.back() != L';')
        {
            pathExtPartToFind += L';';
        }

        int left{ static_cast<int>(currentPathExt.size()) };
        int right{ static_cast<int>(currentPathExt.size() - 1) };
        bool foundPathExtPart{ false };

        while (!foundPathExtPart && left >= 0)
        {
            if (left == 0 || currentPathExt[left - 1] == L';')
            {
                std::wstring pathExtPart(currentPathExt, left, right - left);

                if (CompareStringOrdinal(pathExtPart.c_str(), -1,
                    pathExtPartToFind.c_str(), -1,
                    TRUE) == CSTR_EQUAL)
                {
                    foundPathExtPart = true;
                }
                else
                {
                    right = left;
                }
            }

            left--;
        }

        // foundPathPart is used to check if we need to track and
        // apply the change.
        currentPathExt.erase(left + 1, right - left - 1);

        auto removeFromPathExt = [&, currentPathExt, this]()
        {
            if (m_Scope == Scope::Process)
            {
                THROW_IF_WIN32_BOOL_FALSE(::SetEnvironmentVariable(c_PathExtName, currentPathExt.c_str()));
            }
            else //Scope is either user or machine
            {
                wil::unique_hkey environmentVariableKey{ GetRegHKeyForEVUserAndMachineScope(true) };

                THROW_IF_FAILED(HRESULT_FROM_WIN32(RegSetValueEx(
                    environmentVariableKey.get()
                    , c_PathExtName
                    , 0
                    , REG_EXPAND_SZ
                    , reinterpret_cast<const BYTE*>(currentPathExt.c_str())
                    , static_cast<DWORD>((currentPathExt.size() + 1) * sizeof(wchar_t)))));

                LRESULT broadcastResult{ SendMessageTimeout(HWND_BROADCAST, WM_SETTINGCHANGE,
                    reinterpret_cast<WPARAM>(nullptr), reinterpret_cast<LPARAM>(L"Environment"),
                    SMTO_NOTIMEOUTIFNOTHUNG | SMTO_BLOCK, 1000, nullptr) };

                if (broadcastResult == 0)
                {
                    THROW_HR(HRESULT_FROM_WIN32(GetLastError()));
                }
            }

            return S_OK;
        };

        if (foundPathExtPart)
        {
            PathExtChangeTracker changeTracker(std::wstring(currentPathExt), m_Scope, IChangeTracker::PathOperation::Remove);

            THROW_IF_FAILED(changeTracker.TrackChange(removeFromPathExt));
        }
    }

    std::wstring EnvironmentManager::GetPath() const
    {
        std::wstring path;
        if (m_Scope == Scope::Process)
        {
            path = GetProcessEnvironmentVariable(c_PathName);
        }
        else
        {
            wil::unique_hkey environmentVariableKey{ GetRegHKeyForEVUserAndMachineScope() };

            DWORD sizeOfEnvironmentValue{};

            // See how big we need the buffer to be
            LSTATUS queryResult{ RegQueryValueEx(environmentVariableKey.get(), c_PathName, 0, nullptr, nullptr, &sizeOfEnvironmentValue) };

            if (queryResult == ERROR_FILE_NOT_FOUND)
            {
                path = L"";
            }
            else if (queryResult != ERROR_SUCCESS)
            {
                THROW_HR(HRESULT_FROM_WIN32((queryResult)));
            }

            std::unique_ptr<wchar_t[]> environmentValue(new wchar_t[sizeOfEnvironmentValue]);
            THROW_IF_FAILED(HRESULT_FROM_WIN32((RegQueryValueEx(environmentVariableKey.get(), c_PathName, 0, nullptr, (LPBYTE)environmentValue.get(), &sizeOfEnvironmentValue))));

            path = std::wstring(environmentValue.get());
        }

        if (path.back() != L';')
        {
            path += L';';
        }

        return path;
    }

    std::wstring EnvironmentManager::GetPathExt() const
    {
        std::wstring pathExt;
        if (m_Scope == Scope::Process)
        {
            pathExt = GetProcessEnvironmentVariable(c_PathExtName);
        }
        else
        {
            wil::unique_hkey environmentVariableKey{ GetRegHKeyForEVUserAndMachineScope() };

            DWORD sizeOfEnvironmentValue{};

            // See how big we need the buffer to be
            LSTATUS queryResult{ RegQueryValueEx(environmentVariableKey.get(), c_PathName, 0, nullptr, nullptr, &sizeOfEnvironmentValue) };

            if (queryResult == ERROR_FILE_NOT_FOUND)
            {
                pathExt = L"";
            }
            else if (queryResult != ERROR_SUCCESS)
            {
                THROW_HR(HRESULT_FROM_WIN32((queryResult)));
            }

            std::unique_ptr<wchar_t[]> environmentValue(new wchar_t[sizeOfEnvironmentValue]);
            THROW_IF_FAILED(HRESULT_FROM_WIN32((RegQueryValueEx(environmentVariableKey.get(), c_PathName, 0, nullptr, (LPBYTE)environmentValue.get(), &sizeOfEnvironmentValue))));

            pathExt = std::wstring(environmentValue.get());
        }

        if (pathExt.back() != L';')
        {
            pathExt += L';';
        }

        return pathExt;
    }

    StringMap EnvironmentManager::GetProcessEnvironmentVariables() const
    {
        //Get the pointer to the process block
        PWSTR environmentVariablesString{ GetEnvironmentStrings() };
        THROW_HR_IF_NULL(E_POINTER, environmentVariablesString);

        StringMap environmentVariables;
        for (auto environmentVariableOffset = environmentVariablesString; *environmentVariableOffset; environmentVariableOffset += wcslen(environmentVariableOffset) + 1)
        {
            auto delimiter{ wcschr(environmentVariableOffset, L'=') };
            FAIL_FAST_HR_IF_NULL(E_UNEXPECTED, delimiter);
            std::wstring variableName(environmentVariableOffset, 0, delimiter - environmentVariableOffset);
            auto variableValue{ delimiter + 1 };
            environmentVariables.Insert(variableName, variableValue);
        }

        THROW_IF_WIN32_BOOL_FALSE(FreeEnvironmentStrings(environmentVariablesString));

        return environmentVariables;
    }

    StringMap EnvironmentManager::GetUserOrMachineEnvironmentVariables() const
    {
        StringMap environmentVariables;
        wil::unique_hkey environmentVariablesHKey{ GetRegHKeyForEVUserAndMachineScope() };

        // While this way of calculating the max size of the names,
        // values, and total number of entries includes two calls
        // to the registry, I believe this is superior to
        // using a do/while or a while with a prime
        // because there is no chance of the loop iterating more than
        // is needed AND the size of the name and value arrays are
        // only as big as the biggest name or value.

        DWORD sizeOfLongestNameInCharacters;
        DWORD sizeOfLongestValueInCharacters;
        DWORD numberOfValues;

        THROW_IF_WIN32_ERROR(RegQueryInfoKeyW(environmentVariablesHKey.get(),
            nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
            &numberOfValues, &sizeOfLongestNameInCharacters,
            &sizeOfLongestValueInCharacters, nullptr, nullptr));

        // +1 for null character
        const DWORD c_nameLength{ sizeOfLongestNameInCharacters + 1 };
        const DWORD c_valueSizeInBytes{ static_cast<DWORD>(sizeOfLongestValueInCharacters * sizeof(WCHAR)) };

        std::unique_ptr<wchar_t[]> environmentVariableName(new wchar_t[c_nameLength]);
        std::unique_ptr<BYTE[]> environmentVariableValue(new BYTE[c_valueSizeInBytes]);

        for (DWORD valueIndex = 0; valueIndex < numberOfValues; valueIndex++)
        {
            DWORD nameLength{ c_nameLength };
            DWORD valueSize{ c_valueSizeInBytes };
            LSTATUS enumerationStatus{ RegEnumValueW(environmentVariablesHKey.get(),
                valueIndex, environmentVariableName.get(), &nameLength,
                nullptr, nullptr, environmentVariableValue.get(),
                &valueSize) };

            // An empty name indicates the default value.
            if (nameLength == 0)
            {
                continue;
            }

            // If there was an error getting the value
            if (enumerationStatus != ERROR_SUCCESS && enumerationStatus != ERROR_NO_MORE_ITEMS)
            {
                THROW_HR(HRESULT_FROM_WIN32(enumerationStatus));
            }
            environmentVariableValue.get()[valueSize] = L'\0';
            environmentVariables.Insert(environmentVariableName.get(), reinterpret_cast<PWSTR>(environmentVariableValue.get()));

            environmentVariableName.get()[0] = L'\0';
            environmentVariableValue.reset(new BYTE[c_valueSizeInBytes]);
        }

        return environmentVariables;
    }

    std::wstring EnvironmentManager::GetProcessEnvironmentVariable(const std::wstring variableName) const
    {
        // Get the size of the buffer.
        DWORD sizeNeededInCharacters{ ::GetEnvironmentVariable(variableName.c_str(), nullptr, 0) };

        // If we got an error
        if (sizeNeededInCharacters == 0)
        {
            DWORD lastError{ GetLastError() };

            if (lastError == ERROR_ENVVAR_NOT_FOUND)
            {
                return L"";
            }
            else
            {
                THROW_HR(HRESULT_FROM_WIN32(lastError));
            }
        }

        std::wstring environmentVariableValue{};

        environmentVariableValue.resize(sizeNeededInCharacters - 1);
        DWORD getResult{ ::GetEnvironmentVariable(variableName.c_str(), &environmentVariableValue[0], sizeNeededInCharacters) };

        if (getResult == 0)
        {
            THROW_HR(HRESULT_FROM_WIN32(GetLastError()));
        }

        return environmentVariableValue;
    }

    wil::unique_hkey EnvironmentManager::GetRegHKeyForEVUserAndMachineScope(bool needsWriteAccess) const
    {
        FAIL_FAST_HR_IF(E_INVALIDARG, m_Scope == Scope::Process);

        REGSAM registrySecurity{ KEY_READ };

        if (needsWriteAccess)
        {
            registrySecurity |= KEY_WRITE;
        }

        wil::unique_hkey environmentVariablesHKey{};
        if (m_Scope == Scope::User)
        {
            THROW_IF_FAILED(HRESULT_FROM_WIN32(RegOpenKeyEx(HKEY_CURRENT_USER, c_UserEvRegLocation, 0, registrySecurity, environmentVariablesHKey.addressof())));
        }
        else //Scope is Machine
        {
            THROW_IF_FAILED(HRESULT_FROM_WIN32(RegOpenKeyEx(HKEY_LOCAL_MACHINE, c_MachineEvRegLocation, 0, registrySecurity, environmentVariablesHKey.addressof())));
        }

        return environmentVariablesHKey;
    }

    std::wstring EnvironmentManager::GetUserOrMachineEnvironmentVariable(const std::wstring variableName) const
    {
        wil::unique_hkey environmentVariableHKey{ GetRegHKeyForEVUserAndMachineScope() };

        DWORD sizeOfEnvironmentValue;

        // See how big we need the buffer to be
        LSTATUS queryResult{ RegQueryValueEx(environmentVariableHKey.get(), variableName.c_str(), 0, nullptr, nullptr, &sizeOfEnvironmentValue) };

        if (queryResult != ERROR_SUCCESS)
        {
            if (queryResult == ERROR_FILE_NOT_FOUND)
            {
                return L"";
            }

            THROW_HR(HRESULT_FROM_WIN32((queryResult)));
        }

        std::unique_ptr<wchar_t[]> environmentValue{ new wchar_t[sizeOfEnvironmentValue] };
        THROW_IF_FAILED(HRESULT_FROM_WIN32((RegQueryValueEx(environmentVariableHKey.get(), variableName.c_str(), 0, nullptr, (LPBYTE)environmentValue.get(), &sizeOfEnvironmentValue))));

        return std::wstring(environmentValue.get());
    }

    void EnvironmentManager::DeleteEnvironmentVariableIfExists(const HKEY hkey, const std::wstring name) const
    {
        const auto deleteResult{ RegDeleteValue(hkey, name.c_str()) };

        THROW_HR_IF(HRESULT_FROM_WIN32(deleteResult), (deleteResult != ERROR_SUCCESS) && (deleteResult != ERROR_FILE_NOT_FOUND));
    }
}
