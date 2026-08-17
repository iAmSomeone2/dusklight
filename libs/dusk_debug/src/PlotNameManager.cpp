/**
 * \file PlotNameManager.cpp
 * \brief  
 *
 * \author Brenden Davidson <brenden@bdavidson.dev>
 * \date 2026-08-16
 */

#include "dusk_debug/PlotNameManager.hpp"

using namespace dusk;

PlotNameManager::PlotNameManager() noexcept = default;

PlotNameManager& PlotNameManager::instance() noexcept {
    static PlotNameManager instance;
    return instance;
}

std::shared_ptr<std::string> PlotNameManager::intern_name(
    std::string_view const& name_view) noexcept {
    const auto input_name = std::string(name_view.data(), name_view.size());
    auto& names = this->m_names;

    std::unique_lock lock(this->m_mutex);
    for (const auto& interned_name : names) {
        if (*interned_name == input_name) {
            return interned_name;
        }
    }
    const auto interned_name = std::make_shared<std::string>(input_name);
    names.push_back(interned_name);
    return interned_name;
}
