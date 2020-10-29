/**
 * Copyright (c) 2018, Timothy Stack
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.
 * * Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 * * Neither the name of Timothy Stack nor the names of its contributors
 * may be used to endorse or promote products derived from this software
 * without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ''AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"

#include "lnav.hh"
#include "filter_status_source.hh"

static auto TOGGLE_MSG = "Press " ANSI_BOLD("TAB") " to edit ";
static auto EXIT_MSG = "Press " ANSI_BOLD("TAB") " to exit ";

static auto CREATE_HELP = ANSI_BOLD("i") "/" ANSI_BOLD("o") ": Create in/out";
static auto ENABLE_HELP = ANSI_BOLD("SPC") ": ";
static auto EDIT_HELP = ANSI_BOLD("ENTER") ": Edit";
static auto TOGGLE_HELP = ANSI_BOLD("t") ": To ";
static auto DELETE_HELP = ANSI_BOLD("D") ": Delete";
static auto FILTERING_HELP = ANSI_BOLD("f") ": ";
static auto JUMP_HELP = ANSI_BOLD("ENTER") ": Jump To";

filter_status_source::filter_status_source()
{
    this->tss_fields[TSF_TITLE].set_width(9);
    this->tss_fields[TSF_TITLE].set_role(view_colors::VCR_STATUS_TITLE);
    this->tss_fields[TSF_TITLE].set_value(" Filters ");

    this->tss_fields[TSF_STITCH_TITLE].set_width(2);
    this->tss_fields[TSF_STITCH_TITLE].set_stitch_value(
        view_colors::VCR_STATUS_STITCH_TITLE_TO_NORMAL,
        view_colors::VCR_STATUS_STITCH_NORMAL_TO_TITLE);

    this->tss_fields[TSF_COUNT].set_min_width(16);
    this->tss_fields[TSF_COUNT].set_share(1);
    this->tss_fields[TSF_COUNT].set_role(view_colors::VCR_STATUS);

    this->tss_fields[TSF_FILTERED].set_min_width(20);
    this->tss_fields[TSF_FILTERED].set_share(1);
    this->tss_fields[TSF_FILTERED].set_role(view_colors::VCR_STATUS);

    this->tss_fields[TSF_HELP].right_justify(true);
    this->tss_fields[TSF_HELP].set_width(20);
    this->tss_fields[TSF_HELP].set_value(TOGGLE_MSG);
    this->tss_fields[TSF_HELP].set_left_pad(1);

    this->tss_prompt.set_left_pad(1);
    this->tss_prompt.set_min_width(35);
    this->tss_prompt.set_share(1);
    this->tss_error.set_left_pad(1);
    this->tss_error.set_min_width(35);
    this->tss_error.set_share(1);
}

size_t filter_status_source::statusview_fields()
{
    if (lnav_data.ld_mode == LNM_FILTER) {
        this->tss_fields[TSF_HELP].set_value(EXIT_MSG);
    } else {
        this->tss_fields[TSF_HELP].set_value(TOGGLE_MSG);
    }

    if (this->tss_prompt.empty() && this->tss_error.empty()) {
        lnav_data.ld_view_stack.top() | [this] (auto tc) {
            text_sub_source *tss = tc->get_sub_source();
            if (tss == nullptr) {
                return;
            }

            filter_stack &fs = tss->get_filters();
            auto enabled_count = 0, filter_count = 0;

            for (const auto &tf : fs) {
                if (tf->is_enabled()) {
                    enabled_count += 1;
                }
                filter_count += 1;
            }
            if (filter_count == 0) {
                this->tss_fields[TSF_COUNT].set_value("");
            } else {
                this->tss_fields[TSF_COUNT].set_value(
                    " " ANSI_BOLD("%d")
                    " of " ANSI_BOLD("%d")
                    " enabled ",
                    enabled_count,
                    filter_count);
            }
        };

        return TSF__MAX;
    }

    return 3;
}

status_field &filter_status_source::statusview_value_for_field(int field)
{
    if (field <= 1) {
        return this->tss_fields[field];
    }

    if (!this->tss_error.empty()) {
        return this->tss_error;
    }

    if (!this->tss_prompt.empty()) {
        return this->tss_prompt;
    }

    return this->tss_fields[field];
}

void filter_status_source::update_filtered(text_sub_source *tss)
{
    status_field &sf = this->tss_fields[TSF_FILTERED];

    if (tss == nullptr || tss->get_filtered_count() == 0) {
        if (tss->tss_apply_filters) {
            sf.clear();
        } else {
            sf.set_value(
                " \u2718 Filtering disabled, re-enable with "
                ANSI_BOLD_START ":toggle-filtering" ANSI_NORM);
        }
    }
    else {
        ui_periodic_timer &timer = ui_periodic_timer::singleton();
        attr_line_t &al = sf.get_value();

        if (tss->get_filtered_count() == this->bss_last_filtered_count) {
            if (timer.fade_diff(this->bss_filter_counter) == 0) {
                this->tss_fields[TSF_FILTERED].set_role(
                    view_colors::VCR_STATUS);
                al.with_attr(string_attr(line_range{0, -1}, &view_curses::VC_STYLE, A_BOLD));
            }
        }
        else {
            this->tss_fields[TSF_FILTERED].set_role(
                view_colors::VCR_ALERT_STATUS);
            this->bss_last_filtered_count = tss->get_filtered_count();
            timer.start_fade(this->bss_filter_counter, 3);
        }
        sf.set_value("%'9d Lines not shown", tss->get_filtered_count());
    }
}

filter_help_status_source::filter_help_status_source()
{
    this->fss_help.set_min_width(10);
    this->fss_help.set_share(1);
}

size_t filter_help_status_source::statusview_fields()
{
    lnav_data.ld_view_stack.top() | [this] (auto tc) {
        text_sub_source *tss = tc->get_sub_source();
        if (tss == nullptr) {
            return;
        }

        if (lnav_data.ld_mode == LNM_FILTER) {
            auto &editor = lnav_data.ld_filter_source;
            auto &lv = lnav_data.ld_filter_view;
            auto &fs = tss->get_filters();

            if (editor.fss_editing) {
                auto tf = *(fs.begin() + lv.get_selection());

                if (tf->get_type() == text_filter::type_t::INCLUDE) {
                    this->fss_help.set_value(
                        "                     "
                        "Enter a regular expression to match lines to filter in:");
                } else {
                    this->fss_help.set_value(
                        "                     "
                        "Enter a regular expression to match lines to filter out:");
                }
            } else if (fs.empty()) {
                this->fss_help.set_value("  %s", CREATE_HELP);
            } else {
                auto tf = *(fs.begin() + lv.get_selection());

                this->fss_help.set_value("  %s  %s%s  %s  %s%s  %s  %s%s",
                                         CREATE_HELP,
                                         ENABLE_HELP,
                                         tf->is_enabled() ? "Disable"
                                                          : "Enable ",
                                         EDIT_HELP,
                                         TOGGLE_HELP,
                                         tf->get_type() ==
                                         text_filter::type_t::INCLUDE ?
                                         "OUT" : "IN ",
                                         DELETE_HELP,
                                         FILTERING_HELP,
                                         tss->tss_apply_filters ?
                                         "Disable Filtering" :
                                         "Enable Filtering");
            }
        } else if (lnav_data.ld_mode == LNM_FILES &&
                   lnav_data.ld_session_loaded) {
            if (lnav_data.ld_active_files.fc_files.empty()) {
                this->fss_help.clear();
                return;
            }

            auto &lv = lnav_data.ld_files_view;
            auto sel = lv.get_selection();
            auto &lf = lnav_data.ld_active_files.fc_files[sel];

            this->fss_help.set_value("  %s%s  %s",
                                     ENABLE_HELP,
                                     lf->is_visible() ? "Hide" : "Show",
                                     JUMP_HELP);
        }
    };

    return 1;
}

status_field &filter_help_status_source::statusview_value_for_field(int field)
{
    return this->fss_help;
}
