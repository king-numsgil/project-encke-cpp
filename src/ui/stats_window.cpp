#include "core/pch.hpp"

#include "ui/stats_window.hpp"

#include <algorithm>
#include <cmath>

#include <imgui.h>
#include <implot.h>

namespace encke
{
    namespace
    {
        constexpr ImPlotFlags kPlotFlags =
            ImPlotFlags_NoMenus | ImPlotFlags_NoBoxSelect | ImPlotFlags_NoMouseText;

        // Headroom above the tallest sample, so the peak does not sit on the
        // frame of the plot.
        constexpr f64 kHeadroom = 1.15;

        void row(char const* label, f64 avg, optional<f64> max = nullopt)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(label);
            ImGui::TableNextColumn();
            ImGui::Text("%7.3f", avg);
            ImGui::TableNextColumn();
            if (max.has_value())
            {
                ImGui::Text("%7.3f", *max);
            }
        }
    }

    void StatsWindow::record(f64 now, f64 frame_ms, f64 blocked_ms, span<GpuSection const> gpu)
    {
        bool same_sections = gpu.size() == section_count_;
        for (size_t index = 0; same_sections && index < gpu.size(); ++index)
        {
            same_sections = gpu[index].label == labels_[index];
        }

        // An empty report means "not yet available", not "no sections", so it
        // must not wipe a history that has them.
        if (!gpu.empty() && !same_sections)
        {
            clear();
            section_count_ = std::min(gpu.size(), kMaxSections);
            for (size_t index = 0; index < section_count_; ++index)
            {
                labels_[index] = gpu[index].label;
            }
        }

        if (!started_)
        {
            pending_       = Accumulator{};
            pending_.start = now;
            started_       = true;
        }

        pending_.frames += 1;
        pending_.frame_sum += frame_ms;
        pending_.frame_max = std::max(pending_.frame_max, frame_ms);
        pending_.busy_sum += std::max(frame_ms - blocked_ms, 0.0);

        if (gpu.size() == section_count_ && section_count_ > 0)
        {
            pending_.gpu_frames += 1;
            for (size_t index = 0; index < section_count_; ++index)
            {
                pending_.gpu_sum[index] += gpu[index].ms;
            }
        }

        if (now - pending_.start >= kBucketSeconds)
        {
            push_bucket(now);
        }
    }

    void StatsWindow::push_bucket(f64 now)
    {
        f64 const frames = static_cast<f64>(pending_.frames);

        time_[head_]      = now;
        frame_avg_[head_] = pending_.frame_sum / frames;
        frame_max_[head_] = pending_.frame_max;
        busy_avg_[head_]  = pending_.busy_sum / frames;

        f64 total = 0.0;
        for (size_t index = 0; index < section_count_; ++index)
        {
            f64 const ms = pending_.gpu_frames > 0
                               ? pending_.gpu_sum[index] / static_cast<f64>(pending_.gpu_frames)
                               : 0.0;
            gpu_[index][head_] = ms;
            total += ms;
        }
        gpu_total_[head_] = total;

        head_  = (head_ + 1) % kCapacity;
        count_ = std::min(count_ + 1, kCapacity);

        pending_       = Accumulator{};
        pending_.start = now;
    }

    void StatsWindow::clear()
    {
        head_          = 0;
        count_         = 0;
        section_count_ = 0;
        started_       = false;
    }

    size_t StatsWindow::unwrap()
    {
        size_t const n     = count_;
        size_t const first = count_ < kCapacity ? 0 : head_;

        plot_time_.resize(n);
        plot_frame_avg_.resize(n);
        plot_frame_max_.resize(n);
        plot_busy_.resize(n);
        plot_gpu_total_.resize(n);
        plot_stack_.assign((section_count_ + 1) * n, 0.0);

        for (size_t i = 0; i < n; ++i)
        {
            size_t const at = (first + i) % kCapacity;

            plot_time_[i]      = time_[at];
            plot_frame_avg_[i] = frame_avg_[at];
            plot_frame_max_[i] = frame_max_[at];
            plot_busy_[i]      = busy_avg_[at];
            plot_gpu_total_[i] = gpu_total_[at];

            for (size_t section = 0; section < section_count_; ++section)
            {
                plot_stack_[(section + 1) * n + i] = plot_stack_[section * n + i] + gpu_[section][at];
            }
        }

        return n;
    }

    void StatsWindow::draw_threads(span<Threads const> groups)
    {
        if (groups.empty())
        {
            return;
        }

        size_t total = 0;
        for (Threads const& group : groups)
        {
            total += group.loads.size();
        }

        u64 const now = ThreadLoad::now_ns();

        // A new set of threads starts from its busy time now, so the first
        // frame does not read a whole lifetime as one sample.
        if (loads_.size() != total)
        {
            loads_.clear();
            for (Threads const& group : groups)
            {
                for (ThreadLoad const& load : group.loads)
                {
                    loads_.push_back(LoadSample{.busy_ns = load.busy_ns(now), .at_ns = now});
                }
            }
        }

        f32 const radius  = ImGui::GetTextLineHeight() * 0.35f;
        f32 const spacing = radius * 2.8f;

        ImGui::Separator();

        // SameLine's offset is from the window's edge, not the text's.
        f32 dots_x = 0.0f;
        for (Threads const& group : groups)
        {
            dots_x = std::max(dots_x, ImGui::CalcTextSize(group.name).x);
        }
        dots_x += ImGui::GetCursorPosX() + ImGui::GetStyle().ItemSpacing.x * 2.0f;

        size_t at = 0;
        for (Threads const& group : groups)
        {
            ImGui::TextUnformatted(group.name);
            ImGui::SameLine(dots_x);

            ImVec2 const origin = ImGui::GetCursorScreenPos();
            f32 const    centre_y = origin.y + ImGui::GetTextLineHeight() * 0.5f;
            ImDrawList*  draw     = ImGui::GetWindowDrawList();

            for (size_t index = 0; index < group.loads.size(); ++index, ++at)
            {
                LoadSample& sample = loads_[at];
                u64 const   busy   = group.loads[index].busy_ns(now);

                if (now > sample.at_ns)
                {
                    f64 const elapsed = static_cast<f64>(now - sample.at_ns);
                    f64 const worked  = busy > sample.busy_ns ? static_cast<f64>(busy - sample.busy_ns) : 0.0;
                    f64 const fraction = std::clamp(worked / elapsed, 0.0, 1.0);

                    f64 const blend = 1.0 - std::exp(-elapsed * 1e-9 / kLoadSeconds);
                    sample.fraction += (fraction - sample.fraction) * blend;
                }
                sample.busy_ns = busy;
                sample.at_ns   = now;

                // Green through yellow to red.
                auto const  hue    = static_cast<f32>((1.0 - sample.fraction) / 3.0);
                ImVec2 const centre{origin.x + radius + static_cast<f32>(index) * spacing, centre_y};
                draw->AddCircleFilled(centre, radius, ImColor::HSV(hue, 0.85f, 0.9f));

                if (ImGui::IsMouseHoveringRect(ImVec2{centre.x - radius, centre.y - radius},
                                               ImVec2{centre.x + radius, centre.y + radius}))
                {
                    ImGui::SetTooltip("%s%zu: %.0f%% busy", group.name, index, sample.fraction * 100.0);
                }
            }

            f32 const width = static_cast<f32>(group.loads.size()) * spacing;
            ImGui::Dummy(ImVec2{width, ImGui::GetTextLineHeight()});
            ImGui::SameLine();
            ImGui::Text("%zu queued", group.queued);
        }
    }

    void StatsWindow::draw(Info const& info)
    {
        ImGui::SetNextWindowPos(ImVec2{10.0f, 10.0f}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2{440.0f, 680.0f}, ImGuiCond_FirstUseEver);

        if (!ImGui::Begin("Frame stats"))
        {
            ImGui::End();
            return;
        }

        ImGui::TextUnformatted(info.device);
        ImGui::Text("%ux%u, %s", info.extent.width, info.extent.height, info.present_mode);

        if (info.limit_frames != nullptr)
        {
            char label[48];
            std::snprintf(label, sizeof(label), "limit to %.0f fps", info.limit_hz);
            ImGui::Checkbox(label, info.limit_frames);
        }
        if (info.taa != nullptr)
        {
            ImGui::SameLine();
            ImGui::Checkbox("TAA", info.taa);
        }

        draw_threads(info.threads);

        size_t const n   = unwrap();
        int const    len = static_cast<int>(n);

        if (n == 0)
        {
            ImGui::TextUnformatted("collecting...");
            ImGui::End();
            return;
        }

        f64 const now = plot_time_[n - 1];

        // -- table: means over the last second ---------------------------------
        {
            size_t first = n;
            while (first > 0 && plot_time_[first - 1] > now - kTableSeconds)
            {
                --first;
            }
            f64 const span_count = static_cast<f64>(n - first);

            auto mean = [&](vector<f64> const& values) {
                f64 sum = 0.0;
                for (size_t i = first; i < n; ++i)
                {
                    sum += values[i];
                }
                return sum / span_count;
            };

            f64 frame_worst = 0.0;
            for (size_t i = first; i < n; ++i)
            {
                frame_worst = std::max(frame_worst, plot_frame_max_[i]);
            }

            f64 const frame_mean = mean(plot_frame_avg_);
            ImGui::Separator();
            ImGui::Text("%.0f fps", frame_mean > 0.0 ? 1000.0 / frame_mean : 0.0);

            // Not SizingStretchProp: on the table's first frame no column has
            // a measured width yet, its weights come out 0/0, and the NaN
            // reaches ImGui's content-size maths (UBSan reports it there).
            ImGuiTableFlags const flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchSame;
            if (ImGui::BeginTable("timings", 3, flags))
            {
                ImGui::TableSetupColumn("ms, last 1 s");
                ImGui::TableSetupColumn("mean");
                ImGui::TableSetupColumn("worst");
                ImGui::TableHeadersRow();

                row("frame", frame_mean, frame_worst);
                row("CPU busy", mean(plot_busy_));
                row("GPU", mean(plot_gpu_total_));

                for (size_t section = 0; section < section_count_; ++section)
                {
                    f64 sum = 0.0;
                    for (size_t i = first; i < n; ++i)
                    {
                        sum += plot_stack_[(section + 1) * n + i] - plot_stack_[section * n + i];
                    }

                    char label[64];
                    std::snprintf(label, sizeof(label), "  %s", labels_[section]);
                    row(label, sum / span_count);
                }

                ImGui::EndTable();
            }
        }

        f64 const oldest = now - kPlotSeconds;

        // -- plot: frame time --------------------------------------------------
        {
            f64 peak = 0.0;
            for (size_t i = 0; i < n; ++i)
            {
                if (plot_time_[i] >= oldest)
                {
                    peak = std::max(peak, plot_frame_max_[i]);
                }
            }

            if (ImPlot::BeginPlot("frame time", ImVec2{-1.0f, 180.0f}, kPlotFlags))
            {
                ImPlot::SetupAxes(nullptr, "ms", ImPlotAxisFlags_NoTickLabels, 0);
                ImPlot::SetupAxisLimits(ImAxis_X1, oldest, now, ImPlotCond_Always);
                ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0, std::max(peak * kHeadroom, 0.1),
                                        ImPlotCond_Always);
                ImPlot::SetupLegend(ImPlotLocation_NorthWest);

                // Same label as the mean line, so the band takes its colour
                // and shares its legend entry.
                ImPlotSpec band;
                band.FillAlpha = 0.25f;
                ImPlot::PlotShaded("frame", plot_time_.data(), plot_frame_avg_.data(),
                                   plot_frame_max_.data(), len, band);
                ImPlot::PlotLine("frame", plot_time_.data(), plot_frame_avg_.data(), len);
                ImPlot::PlotLine("CPU busy", plot_time_.data(), plot_busy_.data(), len);
                ImPlot::PlotLine("GPU", plot_time_.data(), plot_gpu_total_.data(), len);

                ImPlot::EndPlot();
            }
        }

        // -- plot: GPU sections, stacked -------------------------------------
        if (section_count_ > 0)
        {
            f64 peak = 0.0;
            for (size_t i = 0; i < n; ++i)
            {
                if (plot_time_[i] >= oldest)
                {
                    peak = std::max(peak, plot_stack_[section_count_ * n + i]);
                }
            }

            if (ImPlot::BeginPlot("GPU", ImVec2{-1.0f, 200.0f}, kPlotFlags))
            {
                ImPlot::SetupAxes(nullptr, "ms", ImPlotAxisFlags_NoTickLabels, 0);
                ImPlot::SetupAxisLimits(ImAxis_X1, oldest, now, ImPlotCond_Always);
                ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0, std::max(peak * kHeadroom, 0.01),
                                        ImPlotCond_Always);
                ImPlot::SetupLegend(ImPlotLocation_NorthWest);

                for (size_t section = 0; section < section_count_; ++section)
                {
                    ImPlot::PlotShaded(labels_[section], plot_time_.data(),
                                       &plot_stack_[section * n], &plot_stack_[(section + 1) * n],
                                       len);
                }

                ImPlot::EndPlot();
            }
        }
        else
        {
            ImGui::TextUnformatted("GPU timestamps unavailable");
        }

        ImGui::End();
    }
}
