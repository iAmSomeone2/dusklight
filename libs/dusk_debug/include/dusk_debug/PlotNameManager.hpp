/**
 * \file PlotNameManager.hpp
 * \brief  
 *
 * \author Brenden Davidson <brenden@bdavidson.dev>
 * \date 2026-08-16
 */

#pragma once

#include <string>
#include <string_view>
#include <memory>
#include <mutex>
#include <vector>

namespace dusk {

class PlotNameManager {
    std::mutex m_mutex;

    /// Registered name set
    std::vector<std::shared_ptr<std::string>> m_names{};
protected:
    PlotNameManager() noexcept;
public:
    PlotNameManager(PlotNameManager&) = delete;
    PlotNameManager(PlotNameManager&&) = delete;
    PlotNameManager& operator=(PlotNameManager&) = delete;
    PlotNameManager& operator=(PlotNameManager&&) = delete;

    /// Gets the singleton instance of DebugNameManager
    static PlotNameManager& instance() noexcept;

    /// Interns the given name so that Tracy gets always gets the same address during a run.
    std::shared_ptr<std::string> intern_name(std::string_view const& name_view) noexcept;
};

} // dusk
