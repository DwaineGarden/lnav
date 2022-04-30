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
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "view_helpers.hh"

#include "config.h"
#include "environ_vtab.hh"
#include "help-txt.h"
#include "lnav.hh"
#include "lnav.indexing.hh"
#include "pretty_printer.hh"
#include "shlex.hh"
#include "sql_help.hh"
#include "sql_util.hh"
#include "view_helpers.examples.hh"
#include "view_helpers.hist.hh"
#include "vtab_module.hh"

const char* lnav_view_strings[LNV__MAX + 1] = {
    "log",
    "text",
    "help",
    "histogram",
    "db",
    "schema",
    "pretty",
    "spectro",

    nullptr,
};

const char* lnav_view_titles[LNV__MAX] = {
    "LOG",
    "TEXT",
    "HELP",
    "HIST",
    "DB",
    "SCHEMA",
    "PRETTY",
    "SPECTRO",
};

nonstd::optional<lnav_view_t>
view_from_string(const char* name)
{
    if (name == nullptr) {
        return nonstd::nullopt;
    }

    auto view_name_iter
        = std::find_if(std::begin(lnav_view_strings),
                       std::end(lnav_view_strings),
                       [&](const char* v) {
                           return v != nullptr && strcasecmp(v, name) == 0;
                       });

    if (view_name_iter == std::end(lnav_view_strings)) {
        return nonstd::nullopt;
    }

    return lnav_view_t(view_name_iter - lnav_view_strings);
}

static void
open_schema_view()
{
    textview_curses* schema_tc = &lnav_data.ld_views[LNV_SCHEMA];
    std::string schema;

    dump_sqlite_schema(lnav_data.ld_db, schema);

    schema += "\n\n-- Virtual Table Definitions --\n\n";
    schema += ENVIRON_CREATE_STMT;
    schema += vtab_module_schemas;
    for (const auto& vtab_iter : *lnav_data.ld_vtab_manager) {
        schema += "\n" + vtab_iter.second->get_table_statement();
    }

    delete schema_tc->get_sub_source();

    auto* pts = new plain_text_source(schema);
    pts->set_text_format(text_format_t::TF_SQL);

    schema_tc->set_sub_source(pts);
    schema_tc->redo_search();
}

static void
open_pretty_view()
{
    static const char* NOTHING_MSG = "Nothing to pretty-print";

    textview_curses* top_tc = *lnav_data.ld_view_stack.top();
    textview_curses* pretty_tc = &lnav_data.ld_views[LNV_PRETTY];
    textview_curses* log_tc = &lnav_data.ld_views[LNV_LOG];
    textview_curses* text_tc = &lnav_data.ld_views[LNV_TEXT];
    attr_line_t full_text;

    delete pretty_tc->get_sub_source();
    pretty_tc->set_sub_source(nullptr);
    if (top_tc->get_inner_height() == 0) {
        pretty_tc->set_sub_source(new plain_text_source(NOTHING_MSG));
        return;
    }

    if (top_tc == log_tc) {
        logfile_sub_source& lss = lnav_data.ld_log_source;
        bool first_line = true;

        for (vis_line_t vl = log_tc->get_top(); vl <= log_tc->get_bottom();
             ++vl) {
            content_line_t cl = lss.at(vl);
            auto lf = lss.find(cl);
            auto ll = lf->begin() + cl;
            shared_buffer_ref sbr;

            if (!first_line && !ll->is_message()) {
                continue;
            }
            auto ll_start = lf->message_start(ll);
            attr_line_t al;

            vl -= vis_line_t(distance(ll_start, ll));
            lss.text_value_for_line(
                *log_tc,
                vl,
                al.get_string(),
                text_sub_source::RF_FULL | text_sub_source::RF_REWRITE);
            lss.text_attrs_for_line(*log_tc, vl, al.get_attrs());
            if (log_tc->get_hide_fields()) {
                al.apply_hide();
            }

            line_range orig_lr
                = find_string_attr_range(al.get_attrs(), &SA_ORIGINAL_LINE);
            attr_line_t orig_al
                = al.subline(orig_lr.lr_start, orig_lr.length());
            attr_line_t prefix_al = al.subline(0, orig_lr.lr_start);

            data_scanner ds(orig_al.get_string());
            pretty_printer pp(&ds, orig_al.get_attrs());
            attr_line_t pretty_al;
            std::vector<attr_line_t> pretty_lines;

            // TODO: dump more details of the line in the output.
            pp.append_to(pretty_al);
            pretty_al.split_lines(pretty_lines);

            for (auto& pretty_line : pretty_lines) {
                if (pretty_line.empty() && &pretty_line == &pretty_lines.back())
                {
                    break;
                }
                pretty_line.insert(0, prefix_al);
                pretty_line.append("\n");
                full_text.append(pretty_line);
            }

            first_line = false;
        }

        if (!full_text.empty()) {
            full_text.erase(full_text.length() - 1, 1);
        }
    } else if (top_tc == text_tc) {
        auto lf = lnav_data.ld_text_source.current_file();

        for (vis_line_t vl = text_tc->get_top(); vl <= text_tc->get_bottom();
             ++vl) {
            auto ll = lf->begin() + vl;
            shared_buffer_ref sbr;

            lf->read_full_message(ll, sbr);
            data_scanner ds(sbr);
            string_attrs_t sa;
            pretty_printer pp(&ds, sa);

            pp.append_to(full_text);
        }
    }
    auto* pts = new plain_text_source();
    pts->replace_with(full_text);
    pretty_tc->set_sub_source(pts);
    if (lnav_data.ld_last_pretty_print_top != log_tc->get_top()) {
        pretty_tc->set_top(vis_line_t(0));
    }
    lnav_data.ld_last_pretty_print_top = log_tc->get_top();
    pretty_tc->redo_search();
}

static void
build_all_help_text()
{
    if (!lnav_data.ld_help_source.empty()) {
        return;
    }

    attr_line_t all_help_text;
    shlex lexer(help_txt.to_string_fragment());
    std::string sub_help_text;

    lexer.with_ignore_quotes(true).eval(
        sub_help_text, lnav_data.ld_exec_context.ec_global_vars);
    all_help_text.with_ansi_string(sub_help_text);

    std::map<std::string, help_text*> sql_funcs;
    std::map<std::string, help_text*> sql_keywords;

    for (const auto& iter : sqlite_function_help) {
        switch (iter.second->ht_context) {
            case help_context_t::HC_SQL_FUNCTION:
            case help_context_t::HC_SQL_TABLE_VALUED_FUNCTION:
                sql_funcs[iter.second->ht_name] = iter.second;
                break;
            case help_context_t::HC_SQL_KEYWORD:
                sql_keywords[iter.second->ht_name] = iter.second;
                break;
            default:
                break;
        }
    }

    for (const auto& iter : sql_funcs) {
        all_help_text.append(2, '\n');
        format_help_text_for_term(*iter.second, 79, all_help_text);
        if (!iter.second->ht_example.empty()) {
            all_help_text.append(1, '\n');
            format_example_text_for_term(
                *iter.second, eval_example, 90, all_help_text);
        }
    }

    for (const auto& iter : sql_keywords) {
        all_help_text.append(2, '\n');
        format_help_text_for_term(*iter.second, 79, all_help_text);
        if (!iter.second->ht_example.empty()) {
            all_help_text.append(1, '\n');
            format_example_text_for_term(
                *iter.second, eval_example, 79, all_help_text);
        }
    }

    lnav_data.ld_help_source.replace_with(all_help_text);
    lnav_data.ld_views[LNV_HELP].redo_search();
}

void
layout_views()
{
    unsigned long width, height;

    getmaxyx(lnav_data.ld_window, height, width);
    int doc_height;
    bool doc_side_by_side = width > (90 + 60);
    bool preview_status_open
        = !lnav_data.ld_preview_status_source.get_description().empty();
    bool filter_status_open = false;

    lnav_data.ld_view_stack.top() | [&](auto tc) {
        text_sub_source* tss = tc->get_sub_source();

        if (tss == nullptr) {
            return;
        }

        if (tss->tss_supports_filtering) {
            filter_status_open = true;
        }
    };

    if (doc_side_by_side) {
        doc_height = std::max(lnav_data.ld_doc_source.text_line_count(),
                              lnav_data.ld_example_source.text_line_count());
    } else {
        doc_height = lnav_data.ld_doc_source.text_line_count()
            + lnav_data.ld_example_source.text_line_count();
    }

    int preview_height = lnav_data.ld_preview_hidden
        ? 0
        : lnav_data.ld_preview_source.text_line_count();
    int match_rows = lnav_data.ld_match_source.text_line_count();
    int match_height = std::min((unsigned long) match_rows, (height - 4) / 2);

    lnav_data.ld_match_view.set_height(vis_line_t(match_height));

    int um_rows = lnav_data.ld_user_message_source.text_line_count();
    if (um_rows > 0
        && std::chrono::steady_clock::now()
            > lnav_data.ld_user_message_expiration)
    {
        lnav_data.ld_user_message_source.clear();
        um_rows = 0;
    }
    int um_height = std::min((unsigned long) um_rows, (height - 4) / 2);

    lnav_data.ld_user_message_view.set_height(vis_line_t(um_height));

    if (doc_height + 14
        > ((int) height - match_height - um_height - preview_height - 2))
    {
        preview_height = 0;
        preview_status_open = false;
    }

    if (doc_height + 14 > ((int) height - match_height - um_height - 2)) {
        doc_height = lnav_data.ld_doc_source.text_line_count();
        if (doc_height + 14 > ((int) height - match_height - um_height - 2)) {
            doc_height = 0;
        }
    }

    bool doc_open = doc_height > 0;
    bool filters_open = (lnav_data.ld_mode == ln_mode_t::FILTER
                         || lnav_data.ld_mode == ln_mode_t::FILES
                         || lnav_data.ld_mode == ln_mode_t::SEARCH_FILTERS
                         || lnav_data.ld_mode == ln_mode_t::SEARCH_FILES)
        && !preview_status_open && !doc_open;
    int filter_height = filters_open ? 5 : 0;

    int bottom_height = (doc_open ? 1 : 0) + doc_height
        + (preview_status_open ? 1 : 0) + preview_height + 1  // bottom status
        + match_height + um_height + lnav_data.ld_rl_view->get_height();

    for (auto& tc : lnav_data.ld_views) {
        tc.set_height(vis_line_t(-(bottom_height + (filter_status_open ? 1 : 0)
                                   + (filters_open ? 1 : 0) + filter_height)));
    }
    lnav_data.ld_status[LNS_TOP].set_enabled(!filters_open);
    lnav_data.ld_status[LNS_FILTER].set_visible(filter_status_open);
    lnav_data.ld_status[LNS_FILTER].set_enabled(filters_open);
    lnav_data.ld_status[LNS_FILTER].set_top(
        -(bottom_height + filter_height + 1 + (filters_open ? 1 : 0)));
    lnav_data.ld_status[LNS_FILTER_HELP].set_visible(filters_open);
    lnav_data.ld_status[LNS_FILTER_HELP].set_top(
        -(bottom_height + filter_height + 1));
    lnav_data.ld_status[LNS_BOTTOM].set_top(-(match_height + um_height + 2));
    lnav_data.ld_status[LNS_DOC].set_top(height - bottom_height);
    lnav_data.ld_status[LNS_DOC].set_visible(doc_open);
    lnav_data.ld_status[LNS_PREVIEW].set_top(height - bottom_height
                                             + (doc_open ? 1 : 0) + doc_height);
    lnav_data.ld_status[LNS_PREVIEW].set_visible(preview_status_open);

    if (!doc_open || doc_side_by_side) {
        lnav_data.ld_doc_view.set_height(vis_line_t(doc_height));
    } else {
        lnav_data.ld_doc_view.set_height(
            vis_line_t(lnav_data.ld_doc_source.text_line_count()));
    }
    lnav_data.ld_doc_view.set_y(height - bottom_height + 1);

    if (!doc_open || doc_side_by_side) {
        lnav_data.ld_example_view.set_height(vis_line_t(doc_height));
        lnav_data.ld_example_view.set_x(doc_open ? 90 : 0);
        lnav_data.ld_example_view.set_y(height - bottom_height + 1);
    } else {
        lnav_data.ld_example_view.set_height(
            vis_line_t(lnav_data.ld_example_source.text_line_count()));
        lnav_data.ld_example_view.set_x(0);
        lnav_data.ld_example_view.set_y(
            height - bottom_height + lnav_data.ld_doc_view.get_height() + 1);
    }

    lnav_data.ld_filter_view.set_height(vis_line_t(filter_height));
    lnav_data.ld_filter_view.set_y(height - bottom_height - filter_height);
    lnav_data.ld_filter_view.set_width(width);

    lnav_data.ld_files_view.set_height(vis_line_t(filter_height));
    lnav_data.ld_files_view.set_y(height - bottom_height - filter_height);
    lnav_data.ld_files_view.set_width(width);

    lnav_data.ld_preview_view.set_height(vis_line_t(preview_height));
    lnav_data.ld_preview_view.set_y(height - bottom_height + 1
                                    + (doc_open ? 1 : 0) + doc_height);
    lnav_data.ld_user_message_view.set_y(
        height - lnav_data.ld_rl_view->get_height() - match_height - um_height);
    lnav_data.ld_match_view.set_y(height - lnav_data.ld_rl_view->get_height()
                                  - match_height);
    lnav_data.ld_rl_view->set_width(width);
}

void
update_hits(textview_curses* tc)
{
    if (isendwin()) {
        return;
    }

    auto top_tc = lnav_data.ld_view_stack.top();

    if (top_tc && tc == *top_tc) {
        lnav_data.ld_bottom_source.update_hits(tc);

        if (lnav_data.ld_mode == ln_mode_t::SEARCH) {
            const auto MAX_MATCH_COUNT = 10_vl;
            const auto PREVIEW_SIZE = MAX_MATCH_COUNT + 1_vl;

            int preview_count = 0;

            vis_bookmarks& bm = tc->get_bookmarks();
            const auto& bv = bm[&textview_curses::BM_SEARCH];
            auto vl = tc->get_top();
            unsigned long width;
            vis_line_t height;
            attr_line_t all_matches;
            char linebuf[64];
            int last_line = tc->get_inner_height();
            int max_line_width;

            snprintf(linebuf, sizeof(linebuf), "%d", last_line);
            max_line_width = strlen(linebuf);

            tc->get_dimensions(height, width);
            vl += height;
            if (vl > PREVIEW_SIZE) {
                vl -= PREVIEW_SIZE;
            }

            auto prev_vl = bv.prev(tc->get_top());

            if (prev_vl != -1_vl) {
                attr_line_t al;

                tc->textview_value_for_row(prev_vl, al);
                if (preview_count > 0) {
                    all_matches.append("\n");
                }
                snprintf(linebuf,
                         sizeof(linebuf),
                         "L%*d: ",
                         max_line_width,
                         (int) prev_vl);
                all_matches.append(linebuf).append(al);
                preview_count += 1;
            }

            while ((vl = bv.next(vl)) != -1_vl
                   && preview_count < MAX_MATCH_COUNT) {
                attr_line_t al;

                tc->textview_value_for_row(vl, al);
                if (preview_count > 0) {
                    all_matches.append("\n");
                }
                snprintf(linebuf,
                         sizeof(linebuf),
                         "L%*d: ",
                         max_line_width,
                         (int) vl);
                all_matches.append(linebuf).append(al);
                preview_count += 1;
            }

            if (preview_count > 0) {
                lnav_data.ld_preview_status_source.get_description().set_value(
                    "Matching lines for search");
                lnav_data.ld_preview_source.replace_with(all_matches)
                    .set_text_format(text_format_t::TF_UNKNOWN);
                lnav_data.ld_preview_view.set_needs_update();
            }
        }
    }
}

static std::unordered_map<std::string, attr_line_t> EXAMPLE_RESULTS;

void
execute_examples()
{
    db_label_source& dls = lnav_data.ld_db_row_source;
    db_overlay_source& dos = lnav_data.ld_db_overlay;
    textview_curses& db_tc = lnav_data.ld_views[LNV_DB];

    for (auto& help_iter : sqlite_function_help) {
        struct help_text& ht = *(help_iter.second);

        for (auto& ex : ht.ht_example) {
            std::string alt_msg;
            attr_line_t result;

            if (!ex.he_cmd) {
                continue;
            }

            switch (ht.ht_context) {
                case help_context_t::HC_SQL_KEYWORD:
                case help_context_t::HC_SQL_INFIX:
                case help_context_t::HC_SQL_FUNCTION:
                case help_context_t::HC_SQL_TABLE_VALUED_FUNCTION: {
                    exec_context ec;

                    execute_sql(ec, ex.he_cmd, alt_msg);

                    if (dls.dls_rows.size() == 1 && dls.dls_rows[0].size() == 1)
                    {
                        result.append(dls.dls_rows[0][0]);
                    } else {
                        attr_line_t al;
                        dos.list_value_for_overlay(db_tc, 0, 1, 0_vl, al);
                        result.append(al);
                        for (int lpc = 0; lpc < (int) dls.text_line_count();
                             lpc++) {
                            al.clear();
                            dls.text_value_for_line(
                                db_tc, lpc, al.get_string(), false);
                            dls.text_attrs_for_line(db_tc, lpc, al.get_attrs());
                            std::replace(al.get_string().begin(),
                                         al.get_string().end(),
                                         '\n',
                                         ' ');
                            result.append("\n").append(al);
                        }
                    }

                    EXAMPLE_RESULTS[ex.he_cmd] = result;

                    log_debug("example: %s", ex.he_cmd);
                    log_debug("example result: %s",
                              result.get_string().c_str());
                    break;
                }
                default:
                    log_warning("Not executing example: %s", ex.he_cmd);
                    break;
            }
        }
    }

    dls.clear();
}

attr_line_t
eval_example(const help_text& ht, const help_example& ex)
{
    auto iter = EXAMPLE_RESULTS.find(ex.he_cmd);

    if (iter != EXAMPLE_RESULTS.end()) {
        return iter->second;
    }

    return "";
}

bool
toggle_view(textview_curses* toggle_tc)
{
    textview_curses* tc = lnav_data.ld_view_stack.top().value_or(nullptr);
    bool retval = false;

    require(toggle_tc != nullptr);
    require(toggle_tc >= &lnav_data.ld_views[0]);
    require(toggle_tc < &lnav_data.ld_views[LNV__MAX]);

    if (tc == toggle_tc) {
        if (lnav_data.ld_view_stack.size() == 1) {
            return false;
        }
        lnav_data.ld_last_view = tc;
        lnav_data.ld_view_stack.pop_back();
    } else {
        if (toggle_tc == &lnav_data.ld_views[LNV_SCHEMA]) {
            open_schema_view();
        } else if (toggle_tc == &lnav_data.ld_views[LNV_PRETTY]) {
            open_pretty_view();
        } else if (toggle_tc == &lnav_data.ld_views[LNV_HISTOGRAM]) {
            // Rebuild to reflect changes in marks.
            rebuild_hist();
        } else if (toggle_tc == &lnav_data.ld_views[LNV_HELP]) {
            build_all_help_text();
        }
        lnav_data.ld_last_view = nullptr;
        lnav_data.ld_view_stack.push_back(toggle_tc);
        retval = true;
    }

    return retval;
}

/**
 * Ensure that the view is on the top of the view stack.
 *
 * @param expected_tc The text view that should be on top.
 * @return True if the view was already on the top of the stack.
 */
bool
ensure_view(textview_curses* expected_tc)
{
    textview_curses* tc = lnav_data.ld_view_stack.top().value_or(nullptr);
    bool retval = true;

    if (tc != expected_tc) {
        toggle_view(expected_tc);
        retval = false;
    }
    return retval;
}

bool
ensure_view(lnav_view_t expected)
{
    return ensure_view(&lnav_data.ld_views[expected]);
}

nonstd::optional<vis_line_t>
next_cluster(vis_line_t (bookmark_vector<vis_line_t>::*f)(vis_line_t) const,
             const bookmark_type_t* bt,
             const vis_line_t top)
{
    textview_curses* tc = get_textview_for_mode(lnav_data.ld_mode);
    vis_bookmarks& bm = tc->get_bookmarks();
    bookmark_vector<vis_line_t>& bv = bm[bt];
    bool top_is_marked = binary_search(bv.begin(), bv.end(), top);
    vis_line_t last_top(top), new_top(top), tc_height;
    unsigned long tc_width;
    int hit_count = 0;

    tc->get_dimensions(tc_height, tc_width);

    while ((new_top = (bv.*f)(new_top)) != -1) {
        int diff = new_top - last_top;

        hit_count += 1;
        if (!top_is_marked || diff > 1) {
            return new_top;
        }
        if (hit_count > 1 && std::abs(new_top - top) >= tc_height) {
            return vis_line_t(new_top - diff);
        }
        if (diff < -1) {
            last_top = new_top;
            while ((new_top = (bv.*f)(new_top)) != -1) {
                if ((std::abs(last_top - new_top) > 1)
                    || (hit_count > 1
                        && (std::abs(top - new_top) >= tc_height)))
                {
                    break;
                }
                last_top = new_top;
            }
            return last_top;
        }
        last_top = new_top;
    }

    if (last_top != top) {
        return last_top;
    }

    return nonstd::nullopt;
}

bool
moveto_cluster(vis_line_t (bookmark_vector<vis_line_t>::*f)(vis_line_t) const,
               const bookmark_type_t* bt,
               vis_line_t top)
{
    textview_curses* tc = get_textview_for_mode(lnav_data.ld_mode);
    auto new_top = next_cluster(f, bt, top);

    if (!new_top) {
        new_top = next_cluster(
            f, bt, tc->is_selectable() ? tc->get_selection() : tc->get_top());
    }
    if (new_top != -1) {
        tc->get_sub_source()->get_location_history() |
            [new_top](auto lh) { lh->loc_history_append(new_top.value()); };

        if (tc->is_selectable()) {
            tc->set_selection(new_top.value());
        } else {
            tc->set_top(new_top.value());
        }
        return true;
    }

    alerter::singleton().chime();

    return false;
}

void
previous_cluster(const bookmark_type_t* bt, textview_curses* tc)
{
    key_repeat_history& krh = lnav_data.ld_key_repeat_history;
    vis_line_t height, initial_top;
    unsigned long width;

    if (tc->is_selectable()) {
        initial_top = tc->get_selection();
    } else {
        initial_top = tc->get_top();
    }
    auto new_top
        = next_cluster(&bookmark_vector<vis_line_t>::prev, bt, initial_top);

    tc->get_dimensions(height, width);
    if (krh.krh_count > 1 && initial_top < (krh.krh_start_line - (1.5 * height))
        && (!new_top || ((initial_top - new_top.value()) < height)))
    {
        bookmark_vector<vis_line_t>& bv = tc->get_bookmarks()[bt];
        new_top = bv.next(std::max(0_vl, initial_top - height));
    }

    if (new_top) {
        tc->get_sub_source()->get_location_history() |
            [new_top](auto lh) { lh->loc_history_append(new_top.value()); };

        if (tc->is_selectable()) {
            tc->set_selection(new_top.value());
        } else {
            tc->set_top(new_top.value());
        }
    } else {
        alerter::singleton().chime();
    }
}

vis_line_t
search_forward_from(textview_curses* tc)
{
    vis_line_t height,
        retval = tc->is_selectable() ? tc->get_selection() : tc->get_top();
    key_repeat_history& krh = lnav_data.ld_key_repeat_history;
    unsigned long width;

    tc->get_dimensions(height, width);

    if (krh.krh_count > 1 && retval > (krh.krh_start_line + (1.5 * height))) {
        retval += vis_line_t(0.90 * height);
    }

    return retval;
}

textview_curses*
get_textview_for_mode(ln_mode_t mode)
{
    switch (mode) {
        case ln_mode_t::SEARCH_FILTERS:
        case ln_mode_t::FILTER:
            return &lnav_data.ld_filter_view;
        case ln_mode_t::SEARCH_FILES:
        case ln_mode_t::FILES:
            return &lnav_data.ld_files_view;
        default:
            return *lnav_data.ld_view_stack.top();
    }
}

hist_index_delegate::hist_index_delegate(hist_source2& hs, textview_curses& tc)
    : hid_source(hs), hid_view(tc)
{
}

void
hist_index_delegate::index_start(logfile_sub_source& lss)
{
    this->hid_source.clear();
}

void
hist_index_delegate::index_line(logfile_sub_source& lss,
                                logfile* lf,
                                logfile::iterator ll)
{
    if (ll->is_continued() || ll->get_time() == 0) {
        return;
    }

    hist_source2::hist_type_t ht;

    switch (ll->get_msg_level()) {
        case LEVEL_FATAL:
        case LEVEL_CRITICAL:
        case LEVEL_ERROR:
            ht = hist_source2::HT_ERROR;
            break;
        case LEVEL_WARNING:
            ht = hist_source2::HT_WARNING;
            break;
        default:
            ht = hist_source2::HT_NORMAL;
            break;
    }

    this->hid_source.add_value(ll->get_time(), ht);
    if (ll->is_marked() || ll->is_expr_marked()) {
        this->hid_source.add_value(ll->get_time(), hist_source2::HT_MARK);
    }
}

void
hist_index_delegate::index_complete(logfile_sub_source& lss)
{
    this->hid_view.reload_data();
}
