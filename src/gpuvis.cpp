/*
 * Copyright 2017 Valve Software
 *
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <future>
#include <set>
#include <sys/stat.h>
#include <sys/types.h>
#include <unordered_map>
#include <vector>
#include <array>

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include "GL/gl3w.h"

#define YA_GETOPT_NO_COMPAT_MACRO
#include "ya_getopt.h"
#include "gpuvis.h"
#include "tdopexpr.h"

// https://github.com/ocornut/imgui/issues/88
#if defined( USE_GTK3 )
  #define NOC_FILE_DIALOG_IMPLEMENTATION
  #define NOC_FILE_DIALOG_GTK
  #include "noc_file_dialog.h"
#elif defined( WIN32 )
  #define NOC_FILE_DIALOG_IMPLEMENTATION
  #define NOC_FILE_DIALOG_WIN32
  #include "noc_file_dialog.h"
#endif

multi_text_color multi_text_color::bright_text = { ImVec4( 1, 1, 0, 1 ) };
multi_text_color multi_text_color::red = { ImVec4( 1, 0, 0, 1 ) };
multi_text_color multi_text_color::def = { ImVec4( 0.90f, 0.90f, 0.90f, 1.00f ) };
multi_text_color multi_text_color::print_text = { ImVec4( 1, 1, 0, 1 ) };

void multi_text_color::update_colors()
{
    def.set( ImGui::GetColorVec4( ImGuiCol_Text ) );
    print_text.set( Clrs::getv4( col_FtracePrintText ) );
    bright_text.set( Clrs::getv4( col_BrightText ) );
}

static bool imgui_input_int( int *val, float w, const char *label, const char *label2, ImGuiInputTextFlags flags = 0 )
{
    bool ret = ImGui::Button( label );

    ImGui::SameLine();
    ImGui::PushItemWidth( imgui_scale( w ) );
    ret |= ImGui::InputInt( label2, val, 0, 0, flags );
    ImGui::PopItemWidth();

    return ret;
}

template < size_t T >
static bool imgui_input_text( const char *button_label, const char *text_label,
                              char ( &buf )[ T ], float w, int flags = 0)
{
    bool ret = ImGui::Button( button_label );

    ImGui::SameLine();
    ImGui::PushItemWidth( imgui_scale( w ) );

    ret |= ImGui::InputText( text_label, buf, T, flags, 0 );

    ImGui::PopItemWidth();

    return ret;
}

static int imgui_ini_save_settings_cb( CIniFile *inifile, int index, const ImGuiIniData &data )
{
    std::string value = string_format( "%.2f,%.2f,%.2f,%.2f",
                                       data.Pos.x, data.Pos.y, data.Size.x, data.Size.y );

    inifile->PutStr( data.Name, value.c_str(), "$imguiwindows$" );
    return 0;
}

static int imgui_ini_load_settings_cb( std::vector< INIEntry > *entries, int index, ImGuiIniData &data )
{
    if ( ( size_t )index < entries->size() )
    {
        const std::string &key = entries->at( index ).first;
        const std::string &val = entries->at( index ).second;

        sscanf( val.c_str(), "%f,%f,%f,%f", &data.Pos.x, &data.Pos.y, &data.Size.x, &data.Size.y );

        data.Collapsed = false;
        data.Name = strdup( key.c_str() );
        return 0;
    }

    return -1;
}

static bool imgui_begin_columns( const char *title,
                                 const std::initializer_list< const char * > &headers,
                                 CIniFile *inifile = NULL, bool *resized = NULL )
{
    bool inited = ImGui::BeginColumns( title, headers.size() );

    for ( const char *str : headers )
    {
        ImGui::TextColored( ImVec4( 1, 1, 0, 1 ), "%s", str );
        ImGui::NextColumn();
    }
    ImGui::Separator();

    // If we were just initialized or resized...
    if ( inifile && ( inited || ( *resized && ImGui::IsMouseReleased( 0 ) ) ) )
    {
        // Go through the columns and save/restore the column width.
        // Skip the last column - it should size to edge of window.
        for ( size_t i = 0; i < headers.size() - 1; i++ )
        {
            std::string key = string_format( "column_width_%s%lu", title, i );

            if ( inited )
            {
                // Try to restore the column widths
                float val = inifile->GetFloat( key.c_str(), -1.0f );
                if ( val <= 0.0f )
                    break;

                ImGui::SetColumnWidth( i, val );
            }
            else
            {
                // Save the column widths
                inifile->PutFloat( key.c_str(), ImGui::GetColumnWidth( i ) );
            }
        }

        // Clear the resized flag
        *resized = false;
    }

    return inited;
}

/*
 * StrPool
 */
const char *StrPool::getstr( const char *str, size_t len )
{
    uint32_t hashval = fnv_hashstr32( str, len );
    const std::string *ret = m_pool.get_val( hashval );

    if ( !ret )
    {
        if ( len == ( size_t )-1 )
            len = strlen( str );
        ret = m_pool.get_val( hashval, std::string( str, len ) );
    }
    return ret->c_str();
}

const char *StrPool::findstr( uint32_t hashval )
{
    std::string *str = m_pool.get_val( hashval );

    return str ? str->c_str() : NULL;
}

/*
 * TraceLoader
 */
TraceLoader::state_t TraceLoader::get_state()
{
    return ( state_t )SDL_AtomicGet( &m_state );
}

bool TraceLoader::is_loading()
{
    return ( get_state() == State_Loading || get_state() == State_CancelLoading );
}

void TraceLoader::set_state( state_t state )
{
    m_filename = "";
    m_trace_events = NULL;
    m_thread = NULL;

    SDL_AtomicSet( &m_state, state );
}

void TraceLoader::cancel_load_file()
{
    // Switch to cancel loading if we're currently loading
    SDL_AtomicCAS( &m_state, State_Loading, State_CancelLoading );
}

bool TraceLoader::load_file( const char *filename )
{
    if ( filename != m_trace_file )
        strcpy_safe( m_trace_file, filename );

    if ( is_loading() )
    {
        logf( "[Error] %s failed, currently loading %s.", __func__, m_filename.c_str() );
        return false;
    }

    size_t filesize = get_file_size( filename );
    if ( !filesize )
    {
        logf( "[Error] %s (%s) failed: %s", __func__, filename, strerror( errno ) );
        return false;
    }

    std::string title = string_format( "%s (%.2f MB)", filename, filesize / ( 1024.0f * 1024.0f ) );

    // Check if we've already loaded this trace file.
    for ( TraceEvents *events : m_trace_events_list )
    {
        if ( events->m_title == title )
            return true;
    }

    // Close currently opened trace files
    while ( !m_trace_events_list.empty() )
        close_event_file( m_trace_events_list.back(), true );

    set_state( State_Loading );
    m_filename = filename;

    m_trace_events = new TraceEvents;
    m_trace_events->m_filename = filename;
    m_trace_events->m_filesize = filesize;
    m_trace_events->m_title = title;

    SDL_AtomicSet( &m_trace_events->m_eventsloaded, 1 );

    m_thread = SDL_CreateThread( thread_func, "eventloader", ( void * )this );
    if ( m_thread )
    {
        new_event_window( m_trace_events );
        m_trace_events_list.push_back( m_trace_events );
        return true;
    }

    logf( "[Error] %s: SDL_CreateThread failed.", __func__ );

    delete m_trace_events;
    m_trace_events = NULL;

    set_state( State_Idle );
    return false;
}

void TraceLoader::new_event_window( TraceEvents *trace_events )
{
    size_t refcount = 0;
    std::string title = trace_events->m_title;

    for ( int i = ( int )m_trace_windows_list.size() - 1; i >= 0; i-- )
    {
        if ( &m_trace_windows_list[ i ]->m_trace_events == trace_events )
            refcount++;
    }

    if ( refcount )
        title += string_format( " #%lu", refcount + 1 );

    TraceWin *win = new TraceWin( *this, *trace_events, title );

    m_trace_windows_list.push_back( win );
}

void TraceLoader::close_event_file( TraceEvents *trace_events, bool close_file )
{
    for ( int i = ( int )m_trace_windows_list.size() - 1; i >= 0; i-- )
    {
        TraceWin *win = m_trace_windows_list[ i ];

        if ( win->m_open && ( &win->m_trace_events == trace_events ) )
            win->m_open = false;
    }

    if ( close_file )
    {
        for ( size_t i = 0; i < m_trace_events_list.size(); i++ )
        {
            if ( m_trace_events_list[ i ] == trace_events )
            {
                delete trace_events;
                m_trace_events_list.erase( m_trace_events_list.begin() + i );
                break;
            }
        }
    }
}

// See notes at top of gpuvis_graph.cpp for explanation of these events.
static bool is_timeline_event( const trace_event_t &event )
{
    if ( !event.timeline || !event.seqno || !event.context )
        return false;

    return ( event.is_fence_signaled() ||
             !strcmp( event.name, "amdgpu_cs_ioctl" ) ||
             !strcmp( event.name, "amdgpu_sched_run_job" ) );
}

int TraceLoader::init_new_event( trace_event_t &event, const trace_info_t &info )
{
    if ( event.id == 0 )
    {
        m_trace_events->m_ts_min = event.ts;
    }

    if ( m_trace_events->m_cpucount.empty() )
    {
        m_trace_events->m_trace_info = info;
        m_trace_events->m_cpucount.resize( info.cpus, 0 );
    }

    if ( event.cpu < m_trace_events->m_cpucount.size() )
        m_trace_events->m_cpucount[ event.cpu ]++;

    // Record the maximum crtc we've ever seen.
    m_crtc_max = std::max< int >( m_crtc_max, event.crtc );

    event.ts -= m_trace_events->m_ts_min;

    // Make sure our events are cleared
    event.flags &= ~( TRACE_FLAG_FENCE_SIGNALED |
                      TRACE_FLAG_FTRACE_PRINT |
                      TRACE_FLAG_IS_VBLANK |
                      TRACE_FLAG_IS_TIMELINE |
                      TRACE_FLAG_IS_SW_QUEUE |
                      TRACE_FLAG_IS_HW_QUEUE);

    // fence_signaled was renamed to dma_fence_signaled post v4.9
    if ( strstr( event.name, "fence_signaled" ) )
        event.flags |= TRACE_FLAG_FENCE_SIGNALED;
    else if ( !strcmp( event.system, "ftrace-print" ) )
        event.flags |= TRACE_FLAG_FTRACE_PRINT;
    else if ( !strcmp( event.name, "drm_vblank_event" ) )
        event.flags |= TRACE_FLAG_IS_VBLANK;
    else if ( strstr( event.name, "amdgpu_cs_ioctl" ) )
        event.flags |= TRACE_FLAG_IS_SW_QUEUE;
    else if ( strstr( event.name, "amdgpu_sched_run_job" ) )
        event.flags |= TRACE_FLAG_IS_HW_QUEUE;

    // Add this event name to our event locations map
    if ( event.is_vblank() )
        m_trace_events->m_tdopexpr_locations.add_location_str( "$name=drm_vblank_event", event.id );

    // Add this event comm to our comm locations map
    m_trace_events->m_comm_locations.add_location_str( event.comm, event.id );

    if ( is_timeline_event( event ) )
    {
        const std::string gfxcontext = get_event_gfxcontext_str( event );

        // Add this event under the "gfx", "sdma0", etc timeline map
        m_trace_events->m_timeline_locations.add_location_str( event.timeline, event.id );

        // Add this event under our "gfx_ctx_seq" or "sdma0_ctx_seq", etc. map
        m_trace_events->m_gfxcontext_locations.add_location_str( gfxcontext.c_str(), event.id );

        // Grab the event locations for this event context
        const std::vector< uint32_t > *plocs = m_trace_events->get_gfxcontext_locs( gfxcontext.c_str() );
        if ( plocs->size() > 1 )
        {
            // First event.
            const trace_event_t &event0 = m_trace_events->m_events[ plocs->front() ];

            // Event right before the event we just added.
            auto it = plocs->rbegin() + 1;
            const trace_event_t &event_prev = m_trace_events->m_events[ *it ];

            // Assume the user comm is the first comm event in this set.
            event.user_comm = event0.comm;

            // Point the event we just added to the previous event in this series
            event.id_start = event_prev.id;

            if ( event.is_fence_signaled() )
            {
                // Mark all the events in this series as timeline events
                for ( uint32_t idx : *plocs )
                {
                    m_trace_events->m_events[ idx ].flags |= TRACE_FLAG_IS_TIMELINE;
                }
            }
        }
    }

    SDL_AtomicAdd( &m_trace_events->m_eventsloaded, 1 );

    if ( get_state() == State_CancelLoading )
        return 1;

    return 0;
}

int TraceLoader::new_event_cb( TraceLoader *loader, const trace_info_t &info,
                               const trace_event_t &event )
{
    TraceEvents *trace_events = loader->m_trace_events;
    size_t id = trace_events->m_events.size();

    trace_events->m_events.push_back( event );
    trace_events->m_events.back().id = id;

    return loader->init_new_event( trace_events->m_events.back(), info );
}

int SDLCALL TraceLoader::thread_func( void *data )
{
    TraceLoader *loader = ( TraceLoader * )data;
    TraceEvents *trace_events = loader->m_trace_events;
    const char *filename = loader->m_filename.c_str();

    logf( "Reading trace file %s...", filename );

    EventCallback trace_cb = std::bind( new_event_cb, loader, _1, _2 );
    if ( read_trace_file( filename, trace_events->m_strpool, trace_cb ) < 0 )
    {
        logf( "[Error]: read_trace_file(%s) failed.", filename );

        SDL_AtomicSet( &trace_events->m_eventsloaded, -1 );
        loader->set_state( State_Idle );
        return -1;
    }

    logf( "Events read: %lu", trace_events->m_events.size() );

    SDL_AtomicSet( &trace_events->m_eventsloaded, 0 );
    loader->set_state( State_Loaded );
    return 0;
}

void TraceLoader::init( int argc, char **argv )
{
    m_inifile.Open( "gpuvis", "gpuvis.ini" );

    Clrs::init( m_inifile );

    m_options.resize( OPT_PresetMax );

    m_options[ OPT_TimelineLabels ].opt_bool( "Show gfx timeline labels", "timeline_gfx_labels", true );
    m_options[ OPT_TimelineEvents ].opt_bool( "Show gfx timeline events", "timeline_gfx_events", true );
    m_options[ OPT_TimelineRenderUserSpace ].opt_bool( "Show gfx timeline userspace", "timeline_gfx_userspace", false );

    m_options[ OPT_PrintTimelineLabels ].opt_bool( "Show print timeline labels", "print_timeline_gfx_labels", true );

    m_options[ OPT_GraphOnlyFiltered ].opt_bool( "Graph only filtered events", "graph_only_filtered", false );

    m_options[ OPT_ShowEventList ].opt_bool( "Show Event List", "show_event_list", true );
    m_options[ OPT_ShowEventList ].hidden = true;

    m_options[ OPT_SyncEventListToGraph ].opt_bool( "Sync Event List to graph mouse location", "sync_eventlist_to_graph", true );

    m_options[ OPT_GraphHeight ].opt_float( "Graph Size: %.1f", "graph_height", 0, 0, 1 );
    m_options[ OPT_GraphHeightZoomed ].opt_float( "Zoomed Graph Size: %.1f", "graph_height_zoomed", 0, 0, 1 );
    m_options[ OPT_GraphHeight ].hidden = true;
    m_options[ OPT_GraphHeightZoomed ].hidden = true;

    m_options[ OPT_EventListRowCount ].opt_int( "Event List Size: %.0f", "eventlist_rows", 0, 0, 100 );
    m_options[ OPT_EventListRowCount ].hidden = true;

    m_options[ OPT_Scale ].opt_float( "Font Scale: %.1f", "scale", 2.0f, 0.25f, 6.0f );
    m_options[ OPT_Scale ].hidden = true;

    m_options[ OPT_UseFreetype ].opt_bool( "Use Freetype", "use_freetype", false );
    m_options[ OPT_UseFreetype ].hidden = true;

    m_options[ OPT_DarkTheme ].opt_bool( "Dark Theme", "dark_theme", true );
    m_options[ OPT_DarkTheme ].hidden = true;

    for ( uint32_t i = OPT_RenderCrtc0; i <= OPT_RenderCrtc9; i++ )
    {
        const std::string desc = string_format( "Show drm_vblank_event crtc%d markers", i - OPT_RenderCrtc0 );
        const std::string inikey = string_format( "render_crtc%d", i - OPT_RenderCrtc0 );

        m_options[ i ].opt_bool( desc, inikey, true );
    }

    add_option_graph_rowsize( "gfx", 8 );
    add_option_graph_rowsize( "print", 10 );
    add_option_graph_rowsize( "sdma0" );
    add_option_graph_rowsize( "sdma1" );

    // Create all the entries for the compute shader rows
    for ( uint32_t val = 0; ; val++ )
    {
        std::string str = comp_str_create_val( val );
        if ( str.empty() )
            break;

        add_option_graph_rowsize( str.c_str() );
    }

    // Read option settings from ini file
    for ( size_t i = 0; i < m_options.size(); i++ )
    {
        option_t &opt = m_options[ i ];

        if ( opt.type == OPT_Float )
            opt.valf = m_inifile.GetFloat( opt.inikey.c_str(), opt.valf );
        else
            opt.val = m_inifile.GetInt( opt.inikey.c_str(), opt.val );
    }

    ImGuiIO &io = ImGui::GetIO();
    m_imguiwindow_entries = m_inifile.GetSectionEntries( "$imguiwindows$" );
    io.IniLoadSettingCB = std::bind( imgui_ini_load_settings_cb, &m_imguiwindow_entries, _1, _2 );
    io.IniSaveSettingCB = std::bind( imgui_ini_save_settings_cb, &m_inifile, _1, _2 );

    parse_cmdline( argc, argv );

    imgui_set_custom_style( !!get_opt( OPT_DarkTheme ), Clrs::getalpha( col_ThemeAlpha ) );

    logf( "Welcome to gpuvis\n" );

    logf( "graph shortcuts:" );
    logf( "  shift+click+drag: zoom to selection" );
    logf( "  ctrl+click+drag: select area to see time" );
    logf( "  click+drag: pan graph" );
    logf( "  alt key: hide all graph labels");

    strcpy_safe( m_trace_file, "trace.dat" );

    imgui_set_scale( get_optf( OPT_Scale ) );
}

option_id_t TraceLoader::add_option_graph_rowsize( const char *name, int defval )
{
    option_t opt;
    const char *fullname = name;

    if ( !strncmp( name, "plot:", 5 ) )
        name = fullname + 5;

    std::string descr = string_format( "%s Size:", name );
    std::string key = string_format( "row_%s_size", name );

    descr[ 0 ] = toupper( descr[ 0 ] );

    opt.opt_int( descr, key, defval, 4, 40 );
    opt.hidden = true;

    opt.val = m_inifile.GetInt( opt.inikey.c_str(), opt.val );

    m_name_optid_map.m_map[ fullname ] = ( option_id_t )m_options.size();
    m_options.push_back( opt );

    return ( option_id_t )( m_options.size() - 1 );
}

bool TraceLoader::imgui_opt( option_id_t optid, float w )
{
    bool changed = false;
    option_t &opt = m_options[ optid ];

    ImGui::PushID( optid );

    if ( opt.type == OPT_Float )
    {
        ImGui::PushItemWidth( imgui_scale( w ) );
        changed |= ImGui::SliderFloat( "##opt_float", &opt.valf, opt.valf_min, opt.valf_max, opt.desc.c_str() );
        ImGui::PopItemWidth();
    }
    else if ( opt.type == OPT_Int )
    {
        ImGui::PushItemWidth( imgui_scale( w ) );
        changed |= ImGui::SliderInt( "##opt_int", &opt.val, opt.val_min, opt.val_max, opt.desc.c_str() );
        ImGui::PopItemWidth();
    }
    else
    {
        changed |= ImGui::CheckboxInt( opt.desc.c_str(), &opt.val );
    }

    ImGui::PopID();

    return changed;
}

int TraceLoader::get_opt( option_id_t opt )
{
    return m_options[ opt ].val;
}

float TraceLoader::get_optf( option_id_t opt )
{
    return m_options[ opt ].valf;
}

int TraceLoader::get_opt_crtc( int crtc )
{
    uint32_t val = crtc + OPT_RenderCrtc0;

    return ( val <= OPT_RenderCrtc9 ) ? m_options[ val ].val : 0;
}

void TraceLoader::shutdown()
{
    if ( m_thread )
    {
        // Cancel any file loading going on.
        cancel_load_file();

        // Wait for our thread to die.
        SDL_WaitThread( m_thread, NULL );
        m_thread = NULL;
    }

    set_state( State_Idle );

    for ( TraceWin *win : m_trace_windows_list )
        delete win;
    m_trace_windows_list.clear();

    for ( TraceEvents *events : m_trace_events_list )
        delete events;
    m_trace_events_list.clear();

    // Write option settings to ini file
    for ( size_t i = 0; i < m_options.size(); i++ )
    {
        const option_t &opt = m_options[ i ];

        if ( opt.type == OPT_Float )
            m_inifile.PutFloat( opt.inikey.c_str(), opt.valf );
        else
            m_inifile.PutInt( opt.inikey.c_str(), opt.val );
    }

    Clrs::shutdown( m_inifile );

    m_inifile.Close();
}

void TraceLoader::render()
{
    if ( !m_trace_windows_list.empty() )
    {
        float y = 0;
        float w = ImGui::GetIO().DisplaySize.x;
        float h = ImGui::GetIO().DisplaySize.y / m_trace_windows_list.size();

        for ( int i = ( int )m_trace_windows_list.size() - 1; i >= 0; i-- )
        {
            TraceWin *win = m_trace_windows_list[ i ];

            if ( !win->m_open )
            {
                // Prune closed windows
                delete win;
                m_trace_windows_list.erase( m_trace_windows_list.begin() + i );
            }
        }

        for ( size_t i = 0; i < m_trace_windows_list.size(); i++ )
        {
            TraceWin *win = m_trace_windows_list[ i ];

            ImGui::SetNextWindowPos( ImVec2( 0, y ), ImGuiSetCond_Always );
            ImGui::SetNextWindowSizeConstraints( ImVec2( w, h ), ImVec2( w, h ) );

            win->render();
            y += h;
        }
    }

    if ( m_show_gpuvis_console )
    {
        ImGui::SetNextWindowSize( ImVec2( 600, 800 ), ImGuiSetCond_FirstUseEver );

        render_console();
    }

    if ( m_show_imgui_test_window )
    {
        ImGui::SetNextWindowSize( ImVec2( 800, 600 ), ImGuiSetCond_FirstUseEver );

        ImGui::ShowTestWindow( &m_show_imgui_test_window );
    }

    if ( m_show_imgui_style_editor )
    {
        ImGui::SetNextWindowSize( ImVec2( 800, 600 ), ImGuiSetCond_FirstUseEver );

        ImGui::Begin( "Style Editor", &m_show_imgui_style_editor );
        ImGui::ShowStyleEditor();
        ImGui::End();
    }

    if ( m_show_imgui_metrics_editor )
    {
        ImGui::ShowMetricsWindow( &m_show_imgui_metrics_editor );
    }

    if ( m_show_font_window )
    {
        ImGui::SetNextWindowSize( ImVec2( 800, 600 ), ImGuiSetCond_FirstUseEver );

        ImGui::Begin( "Font Options", &m_show_font_window );
        render_font_options();
        ImGui::End();
    }

    if ( m_show_color_picker )
    {
        ImGui::SetNextWindowSize( ImVec2( 800, 600 ), ImGuiSetCond_FirstUseEver );

        ImGui::Begin( "Color Configuration", &m_show_color_picker );
        render_color_picker();
        ImGui::End();
    }

    if ( m_show_scale_popup && !ImGui::IsPopupOpen( "Display Scaling" ) )
        ImGui::OpenPopup( "Display Scaling" );
    if ( ImGui::BeginPopupModal( "Display Scaling", NULL, ImGuiWindowFlags_AlwaysAutoResize ) )
    {
        ImGui::Text( "Are you running on a high resolution display?" );
        ImGui::Text( " You can update settings in Font Options dialog." );

        ImGui::Separator();

        if ( ImGui::Button( "Yes", ImVec2( 150, 0 ) ) )
        {
            ImGui::CloseCurrentPopup();
            m_show_scale_popup = false;
        }

        ImGui::SameLine();
        if ( ImGui::Button( "No", ImVec2( 150, 0 ) ) )
        {
            m_options[ OPT_Scale ].valf = 1.0f;
            m_font_main.m_changed = true;
            ImGui::CloseCurrentPopup();
            m_show_scale_popup = false;
        }

        ImGui::EndPopup();
    }
}

void TraceLoader::load_fonts()
{
    // Clear all font texture data, ttf data, glyphs, etc.
    ImGui::GetIO().Fonts->Clear();

    // Add main font
    m_font_main.load_font( m_inifile, "$imgui_font_main$", "Proggy Clean (13)", 13.0f );

    // Add small font
    m_font_small.load_font( m_inifile, "$imgui_font_small$", "Proggy Tiny (10)", 10.0f );

    // Reset max rect size for the print events so they'll redo the CalcTextSize for the
    //  print graph row backgrounds (in graph_render_print_timeline).
    for ( TraceEvents *trace_event : m_trace_events_list )
        trace_event->m_rect_size_max_x = -1.0f;

    if ( m_inifile.GetFloat( "scale", -1.0f ) == -1.0f )
    {
        m_inifile.PutFloat( "scale", get_optf( OPT_Scale ) );
        m_show_scale_popup = true;
        m_show_font_window = true;
    }
}

void TraceLoader::get_window_pos( int &x, int &y, int &w, int &h )
{
    x = m_inifile.GetInt( "win_x", SDL_WINDOWPOS_CENTERED );
    y = m_inifile.GetInt( "win_y", SDL_WINDOWPOS_CENTERED );
    w = m_inifile.GetInt( "win_w", 1280 );
    h = m_inifile.GetInt( "win_h", 1024 );
}

void TraceLoader::save_window_pos( int x, int y, int w, int h )
{
    m_inifile.PutInt( "win_x", x );
    m_inifile.PutInt( "win_y", y );
    m_inifile.PutInt( "win_w", w );
    m_inifile.PutInt( "win_h", h );
}

/*
 * GraphRows
 */
const std::vector< GraphRows::graph_rows_info_t > GraphRows::get_hidden_rows_list()
{
    std::vector< graph_rows_info_t > hidden_rows;

    for ( const graph_rows_info_t &rinfo : m_graph_rows_list )
    {
        if ( rinfo.hidden )
            hidden_rows.push_back( rinfo );
    }

    return hidden_rows;
}

void GraphRows::show_row( const std::string &name, graph_rows_show_t show )
{
    if ( show == GraphRows::SHOW_ALL_ROWS )
    {
        m_graph_rows_hide.clear();

        for ( graph_rows_info_t &row_info : m_graph_rows_list )
            row_info.hidden = false;
    }
    else if ( show == GraphRows::SHOW_ROW )
    {
        // Remove this name from the graph_rows_hide array
        auto idx = std::find( m_graph_rows_hide.begin(), m_graph_rows_hide.end(), name );
        if ( idx != m_graph_rows_hide.end() )
            m_graph_rows_hide.erase( idx );

        // Search for it in m_graph_rows_list and mark as not hidden
        for ( size_t i = 0; i < m_graph_rows_list.size(); i++ )
        {
            if ( m_graph_rows_list[ i ].row_name == name )
            {
                m_graph_rows_list[ i ].hidden = false;
                break;
            }
        }
    }
    else
    {
        bool found = false;

        for ( size_t i = 0; i < m_graph_rows_list.size(); i++ )
        {
            if ( found || ( m_graph_rows_list[ i ].row_name == name ) )
            {
                // Add entry to the graph_rows_hide array
                auto idx = std::find( m_graph_rows_hide.begin(), m_graph_rows_hide.end(), m_graph_rows_list[ i ].row_name );
                if ( idx == m_graph_rows_hide.end() )
                    m_graph_rows_hide.push_back( m_graph_rows_list[ i ].row_name );

                // Mark this graph_row as hidden
                m_graph_rows_list[ i ].hidden = true;

                if ( show != GraphRows::HIDE_ROW_AND_ALL_BELOW )
                    break;

                found = true;

            }
        }
    }
}

// Initialize m_graph_rows_list
void GraphRows::init( CIniFile &inifile, TraceEvents &trace_events )
{
    if ( !m_graph_rows_list.empty() )
        return;

    // Order: gfx -> compute -> gfx hw -> compute hw -> sdma -> sdma hw
    TraceEvents::loc_type_t type;
    const std::vector< uint32_t > *plocs;

    if ( ( plocs = trace_events.get_locs( "gfx", &type ) ) )
        m_graph_rows_list.push_back( { type, plocs->size(), "gfx", false } );

    // Andres: full list of compute rings is comp_[1-2].[0-3].[0-8]
    for ( int c0 = 1; c0 < 3; c0++)
    {
        for ( int c1 = 0; c1 < 4; c1++ )
        {
            for ( int c2 = 0; c2 < 9; c2++ )
            {
                std::string str = string_format( "comp_%d.%d.%d", c0, c1, c2 );

                if ( ( plocs = trace_events.get_locs( str.c_str(), &type ) ) )
                    m_graph_rows_list.push_back( { type, plocs->size(), str, false } );
            }
        }
    }

    if ( ( plocs = trace_events.get_locs( "gfx hw", &type ) ) )
        m_graph_rows_list.push_back( { type, plocs->size(), "gfx hw", false } );

    for ( int c0 = 1; c0 < 3; c0++)
    {
        for ( int c1 = 0; c1 < 4; c1++ )
        {
            for ( int c2 = 0; c2 < 9; c2++ )
            {
                std::string str = string_format( "comp_%d.%d.%d hw", c0, c1, c2 );

                if ( ( plocs = trace_events.get_locs( str.c_str(), &type ) ) )
                    m_graph_rows_list.push_back( { type, plocs->size(), str, false } );
            }
        }
    }

    if ( ( plocs = trace_events.get_locs( "sdma0", &type ) ) )
        m_graph_rows_list.push_back( { type, plocs->size(), "sdma0", false } );
    if ( ( plocs = trace_events.get_locs( "sdma1", &type ) ) )
        m_graph_rows_list.push_back( { type, plocs->size(), "sdma1", false } );

    if ( ( plocs = trace_events.get_locs( "sdma0 hw", &type ) ) )
        m_graph_rows_list.push_back( { type, plocs->size(), "sdma0 hw", false } );
    if ( ( plocs = trace_events.get_locs( "sdma1 hw", &type ) ) )
        m_graph_rows_list.push_back( { type, plocs->size(), "sdma1 hw", false } );

    if ( ( plocs = trace_events.get_locs( "print", &type ) ) )
        m_graph_rows_list.push_back( { type, plocs->size(), "print", false } );

    {
        std::vector< INIEntry > entries = inifile.GetSectionEntries( "$graph_plots$" );

        for ( const INIEntry &entry : entries )
        {
            const std::string &plot_name = entry.first;
            const std::vector< std::string > plot_args = string_explode( entry.second, '\t' );

            if ( plot_args.size() == 2 )
            {
                const std::string &plot_filter = plot_args[ 0 ];
                const std::string &plot_scanf = plot_args[ 1 ];

                plocs = trace_events.get_locs( plot_filter.c_str() );
                if ( plocs )
                {
                    GraphPlot &plot = trace_events.get_plot( plot_name.c_str() );

                    if ( plot.init( trace_events, plot_name, plot_filter, plot_scanf ) )
                    {
                        m_graph_rows_list.push_back(
                                { TraceEvents::LOC_TYPE_Plot, plot.m_plotdata.size(), plot_name, false } );
                    }
                }
            }
        }
    }

    std::vector< graph_rows_info_t > comms;
    for ( auto item : trace_events.m_comm_locations.m_locs.m_map )
    {
        uint32_t hashval = item.first;
        const char *comm = trace_events.m_strpool.findstr( hashval );

        comms.push_back( { TraceEvents::LOC_TYPE_Comm, item.second.size(), comm, false } );
    }

    // Sort by count of events
    std::sort( comms.begin(), comms.end(),
               [=]( const graph_rows_info_t &lx, const graph_rows_info_t &rx )
    {
        return rx.event_count < lx.event_count;
    } );

    // Add the sorted comm events to our m_graph_rows_list array
    m_graph_rows_list.insert( m_graph_rows_list.end(), comms.begin(), comms.end() );

    std::string graph_rows_add_str = inifile.GetStr( "graph_rows_add_str", "" );
    if ( !graph_rows_add_str.empty() )
    {
        std::vector< std::string > rows_add = string_explode( graph_rows_add_str, ',' );

        for ( const std::string &name : rows_add )
            add_row( trace_events, name );
    }

    {
        std::vector< INIEntry > entries = inifile.GetSectionEntries( "$graph_rows_move_after$" );

        if ( !entries.empty() )
        {
            // Do the moves twice to handle move dependencies
            for ( int i = 0; i < 2; i++ )
            {
                for ( const INIEntry &entry : entries )
                {
                    std::string name_src = entry.first;

                    // Undo any = replacements we did when saving to ini file
                    string_replace_str( name_src, "**equalsign**", "=" );

                    move_row( name_src, entry.second );
                }
            }
        }
    }

    std::string graph_rows_hide_str = inifile.GetStr( "graph_rows_hide_str", "" );
    if ( !graph_rows_hide_str.empty() )
    {
        m_graph_rows_hide = string_explode( graph_rows_hide_str, ',' );

        for ( graph_rows_info_t &row_info : m_graph_rows_list )
        {
            auto idx = std::find( m_graph_rows_hide.begin(), m_graph_rows_hide.end(), row_info.row_name );

            row_info.hidden = ( idx != m_graph_rows_hide.end() );
        }
    }
}

void GraphRows::rename_row( const char *comm_old, const char *comm_new )
{
    for ( graph_rows_info_t &row_info : m_graph_rows_list )
    {
        if ( row_info.row_name == comm_old )
        {
            row_info.row_name = comm_new;
            break;
        }
    }
}

void GraphRows::add_row( TraceEvents &trace_events, const std::string &name )
{
    TraceEvents::loc_type_t type;
    const std::vector< uint32_t > *plocs = trace_events.get_locs( name.c_str(), &type );
    size_t size = plocs ? plocs->size() : 0;

    // Add expression to our added rows list
    m_graph_rows_add.push_back( name );

    for ( size_t i = 0; i < m_graph_rows_list.size(); i++ )
    {
        // Add this new filter expression before the first comm / tdop expression event we find
        if ( m_graph_rows_list[ i ].type == TraceEvents::LOC_TYPE_Tdopexpr ||
             m_graph_rows_list[ i ].type == TraceEvents::LOC_TYPE_Comm )
        {
            m_graph_rows_list.insert( m_graph_rows_list.begin() + i,
                                        { type, size, name, false } );
            return;
        }
    }

    // Just add to the end.
    m_graph_rows_list.push_back( { type, size, name, false } );
}

void GraphRows::move_row( const std::string &name_src, const std::string &name_dest )
{
    size_t index_src = find_row( name_src );
    size_t index_dest = find_row( name_dest );

    if ( ( index_dest != ( size_t )-1 ) &&
         ( index_src != ( size_t )-1 ) &&
         ( index_src != index_dest ) )
    {
        m_graph_rows_move.m_map[ name_src ] = name_dest;

        m_graph_rows_list.insert( m_graph_rows_list.begin() + index_dest + 1,
                                  m_graph_rows_list[ index_src ] );

        m_graph_rows_list.erase( m_graph_rows_list.begin() + index_src + ( index_src > index_dest ) );
    }
}

// Search in m_graph_rows_list for name. Returns index or -1 if not found.
size_t GraphRows::find_row( const std::string &name, size_t not_found_val )
{
    auto lambda_name_cmp = [ &name ]( const GraphRows::graph_rows_info_t& row_info )
                                        { return row_info.row_name == name; };
    auto idx = std::find_if( m_graph_rows_list.begin(), m_graph_rows_list.end(), lambda_name_cmp );

    return ( idx != m_graph_rows_list.end() ) ? ( idx - m_graph_rows_list.begin() ) : not_found_val;
}

/*
 * TraceWin
 */
int64_t TraceWin::timestr_to_ts( const char *buf, int64_t tsoffset )
{
    double val;

    if ( sscanf( buf, "%lf", &val ) != 1 )
        val = 0.0;

    return tsoffset + ( int64_t )( val * MSECS_PER_SEC );
}

std::string TraceWin::ts_to_timestr( int64_t event_ts, int64_t tsoffset, int precision )
{
    double val = ( event_ts - tsoffset ) * ( 1.0 / MSECS_PER_SEC );

    return string_format( "%.*lf", precision, val );
}


bool TraceWin::rename_comm_event( const char *comm_old, const char *comm_new )
{
    while( isspace( *comm_old ) )
        comm_old++;
    while ( isspace( *comm_new ) )
        comm_new++;

    if ( m_trace_events.rename_comm( comm_old, comm_new ) )
    {
        m_graph.rows.rename_row( comm_old, comm_new );

        m_loader.m_inifile.PutStr( comm_old, comm_new, "$rename_comm$" );
        return true;
    }

    return false;
}

int TraceWin::ts_to_eventid( int64_t ts )
{
    // When running a debug build w/ ASAN on, the lower_bound function is
    //  horribly slow so we cache the timestamp to event ids.
    int *pid = m_ts_to_eventid_cache.get_val( ts );
    if ( pid )
        return *pid;

    trace_event_t x;
    const std::vector< trace_event_t > &events = m_trace_events.m_events;

    x.ts = ts;

    auto eventidx = std::lower_bound( events.begin(), events.end(), x,
        []( const trace_event_t &f1, const trace_event_t &f2 ) {
            return f1.ts < f2.ts;
        } );

    int id = eventidx - events.begin();

    if ( ( size_t )id >= events.size() )
        id = events.size() - 1;

    m_ts_to_eventid_cache.set_val( ts, id );
    return id;
}

int TraceWin::timestr_to_eventid( const char *buf, int64_t tsoffset )
{
    int64_t ts = timestr_to_ts( buf, tsoffset );

    return ts_to_eventid( ts );
}

const char *filter_get_key_func( StrPool *strpool, const char *name, size_t len )
{
    return strpool->getstr( name, len );
}

const char *filter_get_keyval_func( const trace_event_t *event, const char *name, char ( &buf )[ 64 ] )
{
    if ( !strcasecmp( name, "name" ) )
    {
        return event->name;
    }
    else if ( !strcasecmp( name, "comm" ) )
    {
        return event->comm;
    }
    else if ( !strcasecmp( name, "user_comm" ) )
    {
        return event->user_comm;
    }
    else if ( !strcasecmp( name, "id" ) )
    {
        snprintf_safe( buf, "%u", event->id );
        return buf;
    }
    else if ( !strcasecmp( name, "pid" ) )
    {
        snprintf_safe( buf, "%d", event->pid );
        return buf;
    }
    else if ( !strcasecmp( name, "ts" ) )
    {
        snprintf_safe( buf, "%.6f", event->ts * ( 1.0 / MSECS_PER_SEC ) );
        return buf;
    }
    else if ( !strcasecmp( name, "duration" ) )
    {
        snprintf_safe( buf, "%.6f", event->duration * ( 1.0 / MSECS_PER_SEC ) );
        return buf;
    }

    for ( const event_field_t &field : event->fields )
    {
        // We can compare pointers since they're from same string pool
        if ( name == field.key )
            return field.value;
    }

    return "";
}

const std::vector< uint32_t > *TraceEvents::get_tdopexpr_locs( const char *name, std::string *err )
{
    std::vector< uint32_t > *plocs;
    uint32_t hashval = fnv_hashstr32( name );

    // Try to find whatever our name hashed to. Name should be something like:
    //   $name=drm_vblank_event
    plocs = m_tdopexpr_locations.get_locations_u32( hashval );
    if ( plocs )
        return plocs;

    // Not found - check if we've tried and failed with this name before.
    if ( m_failed_commands.find( hashval ) != m_failed_commands.end() )
        return NULL;

    // If the name has a tdop expression variable prefix, try compiling it
    if ( strchr( name, '$' ) )
    {
        std::string errstr;
        tdop_get_key_func get_key_func = std::bind( filter_get_key_func, &m_strpool, _1, _2 );
        class TdopExpr *tdop_expr = tdopexpr_compile( name, get_key_func, errstr );

        if ( !tdop_expr )
        {
            if ( err )
                *err = errstr;
            else
                logf( "[Error] compiling '%s': %s", name, errstr.c_str() );
        }
        else
        {
            for ( trace_event_t &event : m_events )
            {
                const char *ret;
                tdop_get_keyval_func get_keyval_func = std::bind( filter_get_keyval_func, &event, _1, _2 );

                ret = tdopexpr_exec( tdop_expr, get_keyval_func );
                if ( ret[ 0 ] )
                    m_tdopexpr_locations.add_location_u32( hashval, event.id );
            }

            tdopexpr_delete( tdop_expr );
        }
    }

    // Try to find this name/expression again and add to failed list if we miss again
    plocs = m_tdopexpr_locations.get_locations_u32( hashval );
    if ( !plocs )
        m_failed_commands.insert( hashval );

    return plocs;
}

const std::vector< uint32_t > *TraceEvents::get_comm_locs( const char *name )
{
    return m_comm_locations.get_locations_str( name );
}

const std::vector< uint32_t > *TraceEvents::get_timeline_locs( const char *name )
{
    return m_timeline_locations.get_locations_str( name );
}

// Pass a string like "gfx_249_91446"
const std::vector< uint32_t > *TraceEvents::get_gfxcontext_locs( const char *name )
{
    return m_gfxcontext_locations.get_locations_str( name );
}

bool TraceEvents::rename_comm( const char *comm_old, const char *comm_new )
{
    if ( !comm_old[ 0 ] ||
         !comm_new[ 0 ] ||
         !strcasecmp( comm_old, comm_new ) )
    {
        return false;
    }

    const std::vector< uint32_t > *plocs = get_comm_locs( comm_old );

    if ( plocs )
    {
        const char *commstr_old = m_strpool.getstr( comm_old );
        const char *commstr_new = m_strpool.getstr( comm_new );

        for ( trace_event_t &event : m_events )
        {
            if ( event.comm == commstr_old )
                event.comm = commstr_new;
            if ( event.user_comm == commstr_old )
                event.user_comm = commstr_new;

        }

        uint32_t hashval_new = fnv_hashstr32( comm_new );
        uint32_t hashval_old = fnv_hashstr32( comm_old );

        m_comm_locations.m_locs.set_val( hashval_new, *plocs );
        m_comm_locations.m_locs.m_map.erase( hashval_old );
        return true;
    }

    return false;
}

/*
[Compositor Client] Received Idx ###
[Compositor Client] WaitGetPoses Begin ThreadId=####
[Compositor Client] WaitGetPoses End ThreadId=####

[Compositor] Detected dropped frames: ###
[Compositor] frameTimeout( ### ms )
[Compositor] NewFrame idx=####
[Compositor] Predicting( ##.###### ms )
[Compositor] Re-predicting( ##.###### ms )
[Compositor] TimeSinceLastVSync: #.######(#####)

[Compositor Client] PostPresentHandoff Begin
[Compositor Client] PostPresentHandoff End

[Compositor Client] Submit End
[Compositor Client] Submit Left
[Compositor Client] Submit Right
*/
static const char *s_buf_prefixes[] =
{
    "[Compositor Client] Received Idx ",
    "[Compositor Client] WaitGetPoses ",
    "[Compositor] frameTimeout( ",
    "[Compositor] NewFrame idx= ",
    "[Compositor] Predicting( ",
    "[Compositor] Re-predicting( ",
    "[Compositor Client] PostPresentHandoff ",
    "[Compositor Client] Submit ",
    "[Compositor] Present() ",
};

void TraceEvents::calculate_event_print_info()
{
    if ( !m_print_buf_info.m_map.empty() )
        return;

    const std::vector< uint32_t > *plocs = get_tdopexpr_locs( "$name=print" );
    if ( !plocs )
        return;

    uint32_t row_id = 1;
    util_umap< uint32_t, uint32_t > hash_row_map;

    for ( uint32_t idx : *plocs )
    {
        trace_event_t &event = m_events[ idx ];
        const char *buf = get_event_field_val( event.fields, "buf" );

        // If we can find a colon, use everything before it
        const char *buf_end = strrchr( buf, ':' );

        if ( !buf_end )
        {
            // No colon - try to find one of our buf prefixes
            for ( size_t i = 0; i < ARRAY_SIZE( s_buf_prefixes ); i++ )
            {
                size_t len = strlen( s_buf_prefixes[ i ] );
                if ( !strncasecmp( buf, s_buf_prefixes[ i ], len ) )
                {
                    buf_end = buf + len;
                    break;
                }
            }
        }

        if ( !buf_end )
        {
            // Throw all unrecognized events on line 0
            event.graph_row_id = 0;
        }
        else
        {
            // hash our prefix and put em all on their own row with their own color
            uint32_t hashval = fnv_hashstr32( buf, buf_end - buf );
            uint32_t *prow_id = hash_row_map.get_val( hashval, 0 );

            if ( *prow_id == 0 )
                *prow_id = row_id++;

            event.graph_row_id = *prow_id;
        }

        // Add cached print info for this event
        m_print_buf_info.get_val( event.id, { buf, buf_end } );
    }

    m_rect_size_max_x = -1.0f;
}

void TraceEvents::update_event_print_info_rects( float label_sat, float label_alpha )
{
    m_rect_size_max_x = -1.0f;

    for ( auto &entry : m_print_buf_info.m_map )
    {
        trace_event_t &event = m_events[ entry.first ];
        event_print_info_t &print_info = entry.second;

        print_info.rect_size = ImGui::CalcTextSize( print_info.buf );

        m_rect_size_max_x = std::max< float >( print_info.rect_size.x, m_rect_size_max_x );

        if ( !print_info.buf_end )
        {
            event.color = IM_COL32( 0xff, 0, 0, label_alpha * 255 );
        }
        else
        {
            uint32_t hashval = fnv_hashstr32( print_info.buf, print_info.buf_end - print_info.buf );

            event.color = imgui_col_from_hashval( hashval, label_sat, label_alpha );
        }
    }
}

// Go through gfx, sdma0, sdma1, etc. timelines and calculate event durations
void TraceEvents::calculate_event_durations()
{
    std::vector< trace_event_t > &events = m_events;

    std::vector< uint32_t > erase_list;

    for ( auto &timeline_locs : m_timeline_locations.m_locs.m_map )
    {
        uint32_t graph_row_id = 0;
        int64_t last_fence_signaled_ts = 0;
        std::vector< uint32_t > &locs = timeline_locs.second;
        // const char *name = m_strpool.findstr( timeline_locs.first );

        // Erase all timeline events with single entries or no fence_signaled
        locs.erase( std::remove_if( locs.begin(), locs.end(),
                                    [&events]( const uint32_t index )
                                        { return !events[ index ].is_timeline(); } ),
                    locs.end() );

        if ( locs.empty() )
            erase_list.push_back( timeline_locs.first );

        for ( uint32_t index : locs )
        {
            trace_event_t &fence_signaled = events[ index ];

            if ( fence_signaled.is_fence_signaled() &&
                 is_valid_id( fence_signaled.id_start ) )
            {
                trace_event_t &amdgpu_sched_run_job = events[ fence_signaled.id_start ];
                int64_t start_ts = amdgpu_sched_run_job.ts;

                // amdgpu_cs_ioctl   amdgpu_sched_run_job   fence_signaled
                //       |-----------------|---------------------|
                //       |user-->          |hw-->                |
                //                                               |
                //          amdgpu_cs_ioctl  amdgpu_sched_run_job|   fence_signaled
                //                |-----------------|------------|--------|
                //                |user-->          |hwqueue-->  |hw->    |
                //                                                        |

                // Our starting location will be the last fence signaled timestamp or
                //  our amdgpu_sched_run_job timestamp, whichever is larger.
                int64_t hw_start_ts = std::max< int64_t >( last_fence_signaled_ts, amdgpu_sched_run_job.ts );

                // Set duration times
                fence_signaled.duration = fence_signaled.ts - hw_start_ts;
                amdgpu_sched_run_job.duration = hw_start_ts - amdgpu_sched_run_job.ts;

                if ( is_valid_id( amdgpu_sched_run_job.id_start ) )
                {
                    trace_event_t &amdgpu_cs_ioctl = events[ amdgpu_sched_run_job.id_start ];

                    amdgpu_cs_ioctl.duration = amdgpu_sched_run_job.ts - amdgpu_cs_ioctl.ts;

                    start_ts = amdgpu_cs_ioctl.ts;
                }

                // If our start time stamp is greater than the last fence time stamp then
                //  reset our graph row back to the top.
                if ( start_ts > last_fence_signaled_ts )
                    graph_row_id = 0;
                fence_signaled.graph_row_id = graph_row_id++;

                last_fence_signaled_ts = fence_signaled.ts;

                uint32_t hashval = fnv_hashstr32( fence_signaled.user_comm );
                fence_signaled.color = imgui_col_from_hashval( hashval );
            }
        }
    }

    for ( uint32_t hashval : erase_list )
    {
        // Completely erase timeline rows with zero entries.
        m_timeline_locations.m_locs.m_map.erase( hashval );
    }
}

const std::vector< uint32_t > *TraceEvents::get_locs( const char *name, loc_type_t *type )
{
    const std::vector< uint32_t > *plocs = NULL;

    if ( !strcmp( name, "print" ) )
    {
        if ( type )
            *type = LOC_TYPE_Print;
        plocs = get_tdopexpr_locs( "$name=print" );
    }
    else if ( !strncmp( name, "plot:", 5 ) )
    {
        GraphPlot *plot = get_plot_ptr( name );

        if ( plot )
        {
            if ( type )
                *type = LOC_TYPE_Plot;
            plocs = get_tdopexpr_locs( plot->m_filter_str.c_str() );
        }
    }
    else
    {
        size_t len = strlen( name );

        if ( ( len > 3 ) && !strcmp( name + len - 3, " hw" ) )
        {
            // Check for "gfx hw", "comp_1.1.1 hw", etc.
            uint32_t hashval = fnv_hashstr32( name, len - 3 );

            if ( type )
                *type = LOC_TYPE_Timeline_hw;
            plocs = m_timeline_locations.get_locations_u32( hashval );
        }
        else
        {
            // Check for regular comm type rows
            if ( type )
                *type = LOC_TYPE_Comm;
            plocs = get_comm_locs( name );

            if ( !plocs )
            {
                // TDOP Expressions. Ie, $name = print, etc.
                if ( type )
                    *type = LOC_TYPE_Tdopexpr;
                plocs = get_tdopexpr_locs( name );

                if ( !plocs )
                {
                    // Timelines: sdma0, gfx, comp_1.2.1, etc.
                    if ( type )
                        *type = LOC_TYPE_Timeline;
                    plocs = get_timeline_locs( name );
                }
            }
        }
    }

    if ( !plocs && type )
        *type = LOC_TYPE_Max;
    return plocs;
}

TraceWin::TraceWin( TraceLoader &loader, TraceEvents &trace_events, std::string &title ) :
    m_trace_events( trace_events), m_loader( loader )
{
    // Note that m_trace_events is possibly being loaded in
    //  a background thread at this moment, so be sure to check
    //  m_eventsloaded before accessing it...

    m_title = title;

    strcpy_safe( m_eventlist.timegoto_buf, "0.0" );
    strcpy_safe( m_eventlist.timeoffset_buf, "0.0" );

    strcpy_safe( m_graph.new_row_buf, "<Enter Filter Expression>" );

    std::string event_filter = m_loader.m_inifile.GetStr( "event_filter_buf", "" );
    if ( !event_filter.empty() )
    {
        strcpy_safe( m_eventlist.filter_buf, event_filter.c_str() );
        m_eventlist.do_filter = true;
    }
}

TraceWin::~TraceWin()
{
    m_loader.m_inifile.PutStr( "event_filter_buf", m_eventlist.filter_buf );

    std::string str = string_implode( m_graph.rows.m_graph_rows_hide, "," );
    m_loader.m_inifile.PutStr( "graph_rows_hide_str", str.c_str() );

    str = string_implode( m_graph.rows.m_graph_rows_add, "," );
    m_loader.m_inifile.PutStr( "graph_rows_add_str", str.c_str() );

    for ( const auto &item : m_graph.rows.m_graph_rows_move.m_map )
    {
        std::string key = item.first;

        // Can't have equal signs in our ini keys...
        string_replace_str( key, "=", "**equalsign**" );
        m_loader.m_inifile.PutStr( key.c_str(), item.second.c_str(), "$graph_rows_move_after$" );
    }
}

bool TraceWin::render()
{
    int eventsloaded = SDL_AtomicGet( &m_trace_events.m_eventsloaded );

    if ( eventsloaded > 0 )
    {
        ImGui::Begin( m_title.c_str(), &m_open );

        ImGui::Text( "Loading events %u...", eventsloaded );
        if ( ImGui::Button( "Cancel" ) )
            m_loader.cancel_load_file();

        ImGui::End();
        return true;
    }
    else if ( eventsloaded == -1 )
    {
        ImGui::Begin( m_title.c_str(), &m_open );

        ImGui::Text( "Error loading filed %s...\n", m_trace_events.m_filename.c_str() );

        ImGui::End();
        return true;
    }

    ImGui::Begin( m_title.c_str(), &m_open, ImGuiWindowFlags_MenuBar );

    m_loader.render_menu();

    if ( ImGui::CollapsingHeader( "Trace Info" ) )
        trace_render_info();

    if ( m_trace_events.m_events.empty() )
    {
        ImGui::End();
        return true;
    }

    if ( !m_inited )
    {
        std::vector< INIEntry > entries = m_loader.m_inifile.GetSectionEntries( "$rename_comm$" );

        // Init event durations
        m_trace_events.calculate_event_durations();
        // Init print column information
        m_trace_events.calculate_event_print_info();

        // Initialize our graph rows first time through.
        m_graph.rows.init( m_loader.m_inifile, m_trace_events );

        for ( INIEntry &entry : entries )
            rename_comm_event( entry.first.c_str(), entry.second.c_str() );

        int64_t last_ts = m_trace_events.m_events.back().ts;

        m_graph.do_start_timestr = true;
        m_graph.do_length_timestr = true;
        m_graph.length_ts = std::min< int64_t >( last_ts, 40 * MSECS_PER_SEC );
        m_graph.start_ts = last_ts - m_eventlist.tsoffset - m_graph.length_ts;

        m_eventlist.do_gotoevent = true;
        m_eventlist.goto_eventid = ts_to_eventid( m_graph.start_ts + m_graph.length_ts / 2 );
    }

    if ( ImGui::CollapsingHeader( "Events Graph", ImGuiTreeNodeFlags_DefaultOpen ) )
    {
        if ( imgui_input_text( "Start:", "##GraphStart", m_graph.time_start_buf, 150.0f ) )
            m_graph.start_ts = timestr_to_ts( m_graph.time_start_buf );

        ImGui::SameLine();
        if ( imgui_input_text( "Length:", "##GraphLength", m_graph.time_length_buf, 150.0f ) )
            m_graph.length_ts = timestr_to_ts( m_graph.time_length_buf );

        ImGui::SameLine();
        bool m_do_graph_zoom_in = ImGui::SmallButton( "Zoom In" );

        ImGui::SameLine();
        bool m_do_graph_zoom_out = ImGui::SmallButton( "Zoom Out" );

        if ( m_do_graph_zoom_in || m_do_graph_zoom_out )
        {
            int64_t ts0 = m_graph.start_ts + m_graph.length_ts / 2;

            graph_zoom( ts0, m_graph.start_ts, m_do_graph_zoom_in );
        }

        if ( m_graph.do_start_timestr )
            strcpy_safe( m_graph.time_start_buf, ts_to_timestr( m_graph.start_ts, 0, 4 ) );
        if ( m_graph.do_length_timestr )
            strcpy_safe( m_graph.time_length_buf, ts_to_timestr( m_graph.length_ts, 0, 4 ) );

        graph_render_process();
    }

    ImGuiTreeNodeFlags eventslist_flags = m_loader.get_opt( OPT_ShowEventList ) ?
        ImGuiTreeNodeFlags_DefaultOpen : 0;
    m_loader.m_options[ OPT_ShowEventList ].val = ImGui::CollapsingHeader( "Events List", eventslist_flags );
    if ( m_loader.get_opt( OPT_ShowEventList ) )
    {
        m_eventlist.do_gotoevent |= imgui_input_int( &m_eventlist.goto_eventid, 75.0f, "Goto Event:", "##GotoEvent" );

        ImGui::SameLine();
        if ( imgui_input_text( "Goto Time:", "##GotoTime", m_eventlist.timegoto_buf, 150.0f ) )
        {
            m_eventlist.do_gotoevent = true;
            m_eventlist.goto_eventid = timestr_to_eventid( m_eventlist.timegoto_buf, m_eventlist.tsoffset );
        }

        ImGui::SameLine();
        if ( imgui_input_text( "Time Offset:", "##TimeOffset", m_eventlist.timeoffset_buf, 150.0f ) )
            m_eventlist.tsoffset = timestr_to_ts( m_eventlist.timeoffset_buf );

        if ( m_eventlist.do_filter ||
             imgui_input_text( "Event Filter:", "##Event Filter", m_eventlist.filter_buf, 500.0f,
                               ImGuiInputTextFlags_EnterReturnsTrue ) )
        {
            m_eventlist.filtered_events.clear();
            m_eventlist.filtered_events_str.clear();
            m_eventlist.do_filter = false;

            if ( m_eventlist.filter_buf[ 0 ] )
            {
                tdop_get_key_func get_key_func = std::bind( filter_get_key_func, &m_trace_events.m_strpool, _1, _2 );
                class TdopExpr *tdop_expr = tdopexpr_compile( m_eventlist.filter_buf, get_key_func, m_eventlist.filtered_events_str );

                util_time_t t0 = util_get_time();

                if ( tdop_expr )
                {
                    for ( trace_event_t &event : m_trace_events.m_events )
                    {
                        tdop_get_keyval_func get_keyval_func = std::bind( filter_get_keyval_func, &event, _1, _2 );

                        const char *ret = tdopexpr_exec( tdop_expr, get_keyval_func );

                        event.is_filtered_out = !ret[ 0 ];
                        if ( !event.is_filtered_out )
                            m_eventlist.filtered_events.push_back( event.id );
                    }

                    if ( m_eventlist.filtered_events.empty() )
                        m_eventlist.filtered_events_str = "WARNING: No events found.";

                    tdopexpr_delete( tdop_expr );
                    tdop_expr = NULL;
                }

                float time = util_time_to_ms( t0, util_get_time() );
                if ( time > 1000.0f )
                    logf( "tdopexpr_compile(\"%s\"): %.2fms\n", m_eventlist.filter_buf, time );
            }
        }

        if ( ImGui::IsItemHovered() )
        {
            std::string tooltip;

            tooltip += multi_text_color::bright_text.m_str( "Event Filter\n\n" );
            tooltip += "Vars: Any field in Info column plus:\n";
            tooltip += "      $name, $comm, $user_comm, $id, $pid, $ts\n";
            tooltip += "Operators: &&, ||, !=, =, >, >=, <, <=, =~\n\n";

            tooltip += "Examples:\n";
            tooltip += "  $pid = 4615\n";
            tooltip += "  $ts >= 11.1 && $ts < 12.5\n";
            tooltip += "  $ring_name = 0xffff971e9aa6bdd0\n";
            tooltip += "  $buf =~ \"[Compositor] Warp\"\n";
            tooltip += "  ( $timeline = gfx ) && ( $id < 10 || $id > 100 )";

            ImGui::SetTooltip( "%s", tooltip.c_str() );
        }

        ImGui::SameLine();
        if ( ImGui::SmallButton( "Clear Filter" ) )
        {
            m_eventlist.filtered_events.clear();
            m_eventlist.filtered_events_str.clear();
            m_eventlist.filter_buf[ 0 ] = 0;
        }

        if ( !m_eventlist.filtered_events_str.empty() )
        {
            ImGui::SameLine();
            ImGui::TextColored( ImVec4( 1, 0, 0, 1 ), "%s", m_eventlist.filtered_events_str.c_str() );
        }
        else if ( !m_eventlist.filtered_events.empty() )
        {
            std::string label = string_format( "Graph only filtered (%lu events)", m_eventlist.filtered_events.size() );

            ImGui::SameLine();
            m_loader.imgui_opt( OPT_GraphOnlyFiltered );
        }

        events_list_render( m_loader.m_inifile );
    }

    if ( is_valid_id( m_create_plot_eventid ) )
    {
        m_create_plot_dlg.init( m_trace_events, m_create_plot_eventid );
        m_create_plot_eventid = INVALID_ID;
    }
    if ( m_create_plot_dlg.render_dlg( m_trace_events ) )
        m_create_plot_dlg.add_plot( m_loader.m_inifile, m_graph.rows );

    ImGui::End();

    m_inited = true;
    return m_open;
}

void TraceWin::trace_render_info()
{
    size_t event_count = m_trace_events.m_events.size();

    ImGui::Text( "Total Events: %lu\n", event_count );

    if ( event_count )
    {
        const trace_info_t &trace_info = m_trace_events.m_trace_info;
        ImGui::Text( "Trace cpus: %u", trace_info.cpus );

        if ( !trace_info.uname.empty() )
            ImGui::Text( "Trace uname: %s", trace_info.uname.c_str() );

        ImGui::Indent();

        if ( !m_graph.rows.m_graph_rows_list.empty() )
        {
            if ( ImGui::CollapsingHeader( "Graph Row Info", ImGuiTreeNodeFlags_DefaultOpen ) )
            {
                if ( imgui_begin_columns( "row_info", { "Name", "Events" } ) )
                    ImGui::SetColumnWidth( 0, imgui_scale( 200.0f ) );

                for ( const GraphRows::graph_rows_info_t &info : m_graph.rows.m_graph_rows_list )
                {
                    ImGui::Text( "%s", info.row_name.c_str() );
                    ImGui::NextColumn();
                    ImGui::Text( "%lu", info.event_count );

                    if ( info.type == TraceEvents::LOC_TYPE_Plot )
                    {
                        GraphPlot *plot = m_trace_events.get_plot_ptr( info.row_name.c_str() );

                        if ( plot )
                        {
                            ImGui::SameLine();
                            ImGui::Text( "(minval:%.2f maxval:%.2f)", plot->m_minval, plot->m_maxval );
                        }
                    }
                    ImGui::NextColumn();
                }

                ImGui::EndColumns();
            }
        }

        if ( !trace_info.cpustats.empty() )
        {
            if ( ImGui::CollapsingHeader( "CPU Info" ) )
            {
                if ( imgui_begin_columns( "cpu_stats", { "CPU", "Stats" } ) )
                    ImGui::SetColumnWidth( 0, imgui_scale( 200.0f ) );

                for ( const std::string &str : trace_info.cpustats )
                {
                    const char *lf = strchr( str.c_str(), '\n' );

                    if ( lf )
                    {
                        ImGui::Text( "%.*s", ( int )( lf - str.c_str() ), str.c_str() );
                        ImGui::NextColumn();
                        ImGui::Text( "%s", lf + 1 );
                        ImGui::NextColumn();
                        ImGui::Separator();
                    }
                }

                ImGui::EndColumns();
            }
        }

        ImGui::Unindent();
    }
}

bool TraceWin::events_list_render_popupmenu( uint32_t eventid )
{
    if ( !ImGui::BeginPopup( "EventsListPopup" ) )
        return false;

    imgui_text_bg( "Options", ImGui::GetColorVec4( ImGuiCol_Header ) );
    ImGui::Separator();

    trace_event_t &event = get_event( eventid );

    std::string label = string_format( "Center event %u on graph", event.id );
    if ( ImGui::MenuItem( label.c_str() ) )
    {
        m_eventlist.selected_eventid = event.id;
        m_graph.start_ts = event.ts - m_eventlist.tsoffset - m_graph.length_ts / 2;
        m_graph.do_start_timestr = true;
    }

    label = string_format( "Set Time Offset to %s", ts_to_timestr( event.ts ).c_str() );
    if ( ImGui::MenuItem( label.c_str() ) )
    {
        m_eventlist.tsoffset = event.ts;
        strcpy_safe( m_eventlist.timeoffset_buf, ts_to_timestr( m_eventlist.tsoffset ) );
    }

    ImGui::Separator();

    label = string_format( "Filter pid %d events", event.pid );
    if ( ImGui::MenuItem( label.c_str() ) )
    {
        snprintf_safe( m_eventlist.filter_buf, "$pid == %d", event.pid );
        m_eventlist.do_filter = true;
    }

    label = string_format( "Filter '%s' events", event.name );
    if ( ImGui::MenuItem( label.c_str() ) )
    {
        snprintf_safe( m_eventlist.filter_buf, "$name == %s", event.name );
        m_eventlist.do_filter = true;
    }

    if ( !m_eventlist.filtered_events.empty() && ImGui::MenuItem( "Clear Filter" ) )
    {
        m_eventlist.filter_buf[ 0 ] = 0;
        m_eventlist.do_filter = true;
    }

    const char *plot_str = CreatePlotDlg::get_plot_str( event );
    if ( plot_str )
    {
        ImGui::Separator();

        std::string plot_label = std::string( "Create Plot for " ) + multi_text_color::bright_text.m_str( plot_str );
        if ( ImGui::MenuItem( plot_label.c_str() ) )
            m_create_plot_eventid = event.id;
    }

    ImGui::EndPopup();
    return true;
}

std::string get_event_fields_str( const trace_event_t &event, const char *eqstr, char sep )
{
    std::string fieldstr;
    const std::vector< event_field_t > &fields = event.fields;

    if ( event.user_comm != event.comm )
        fieldstr += string_format( "%s%s%s%c", "user_comm", eqstr, event.user_comm, sep );

    for ( const event_field_t &field : fields )
    {
        std::string str = string_format( "%s%s%s%c", field.key, eqstr, field.value, sep );

        if ( event.is_ftrace_print() && !strcmp( field.key, "buf" ) )
            fieldstr += multi_text_color::print_text.m_str( str.c_str() );
        else
            fieldstr += str;
    }

    fieldstr += string_format( "%s%s%s", "system", eqstr, event.system );

    return fieldstr;
}

static float get_keyboard_scroll_lines( float visible_rows )
{
    float scroll_lines = 0.0f;

    if ( ImGui::IsWindowFocused() )
    {
        if ( imgui_key_pressed( ImGuiKey_PageDown ) )
            scroll_lines = std::max< float>( visible_rows - 5, 1 );
        else if ( imgui_key_pressed( ImGuiKey_PageUp ) )
            scroll_lines = std::min< float >( -visible_rows, -1 );
        else if ( imgui_key_pressed( ImGuiKey_DownArrow ) )
            scroll_lines = 1;
        else if ( imgui_key_pressed( ImGuiKey_UpArrow ) )
            scroll_lines = -1;
        else if ( imgui_key_pressed( ImGuiKey_Home ) )
            scroll_lines = -ImGui::GetScrollMaxY();
        else if ( imgui_key_pressed( ImGuiKey_End ) )
            scroll_lines = ImGui::GetScrollMaxY();
    }

    return scroll_lines;
}


bool TraceWin::events_list_handle_mouse( const trace_event_t &event, uint32_t i )
{
    bool popup_shown = false;

    // Check if item is hovered and we don't have a popup menu going already.
    if ( !is_valid_id( m_eventlist.popup_eventid ) &&
         ImGui::IsItemHovered() &&
         ImGui::IsRootWindowOrAnyChildFocused() )
    {
        // Store the hovered event id.
        m_eventlist.hovered_eventid = event.id;

        if ( ImGui::IsMouseClicked( 1 ) )
        {
            // If they right clicked, show the context menu.
            m_eventlist.popup_eventid = i;

            // Open the popup for events_list_render_popupmenu().
            ImGui::OpenPopup( "EventsListPopup" );
        }
        else
        {
            // Otherwise show a tooltip.
            std::string ts_str = ts_to_timestr( event.ts, m_eventlist.tsoffset );
            std::string fieldstr = get_event_fields_str( event, ": ", '\n' );

            ImGui::SetTooltip( "Id: %u\nTime: %s\nComm: %s\n%s",
                               event.id, ts_str.c_str(), event.comm, fieldstr.c_str() );
        }
    }

    // If we've got an active popup menu, render it.
    if ( m_eventlist.popup_eventid == i )
    {
        uint32_t eventid = !m_eventlist.filtered_events.empty() ?
                    m_eventlist.filtered_events[ m_eventlist.popup_eventid ] :
                    m_eventlist.popup_eventid;

        if ( !TraceWin::events_list_render_popupmenu( eventid ) )
            m_eventlist.popup_eventid = INVALID_ID;

        popup_shown = true;
    }

    return popup_shown;
}

static void draw_ts_line( const ImVec2 &pos )
{
    ImGui::PopClipRect();

    float max_x = ImGui::GetWindowClipRectMax().x;
    float spacing_U = ( float )( int )( ImGui::GetStyle().ItemSpacing.y * 0.5f );
    float pos_y = pos.y - spacing_U;

    ImGui::GetWindowDrawList()->AddLine(
                ImVec2( pos.x, pos_y ), ImVec2( max_x, pos_y ),
                Clrs::get( col_MousePos ), imgui_scale( 2.0f ) );

    ImGui::PushColumnClipRect();
}

void TraceWin::events_list_render( CIniFile &inifile )
{
    const std::vector< trace_event_t > &events = m_trace_events.m_events;
    size_t event_count = m_eventlist.filtered_events.empty() ?
                events.size() : m_eventlist.filtered_events.size();

    // Set focus on event list first time we open.
    if ( !m_inited && ImGui::IsWindowFocused() )
        ImGui::SetNextWindowFocus();

    // Events list
    {
        float lineh = ImGui::GetTextLineHeightWithSpacing();
        const ImVec2 content_avail = ImGui::GetContentRegionAvail();

        int eventlist_row_count = m_loader.get_opt( OPT_EventListRowCount );

        // If the user has set the event list row count to 0 (auto size), make
        //  sure we always have at least 20 rows.
        if ( !eventlist_row_count && ( content_avail.y / lineh < 20 ) )
            eventlist_row_count = 20;

        // Set the child window size to hold count of items + header + separator
        float sizey = eventlist_row_count * lineh;

        ImGui::SetNextWindowContentSize( { 0.0f, ( event_count + 1 ) * lineh + 1 } );
        ImGui::BeginChild( "eventlistbox", ImVec2( 0.0f, sizey ) );

        float winh = ImGui::GetWindowHeight();
        uint32_t visible_rows = ( winh + 1 ) / lineh;

        float scroll_lines = get_keyboard_scroll_lines( visible_rows );
        if ( scroll_lines )
            ImGui::SetScrollY( ImGui::GetScrollY() + scroll_lines * lineh );

        bool filtered_events = !m_eventlist.filtered_events.empty();

        if ( m_eventlist.do_gotoevent )
        {
            uint32_t pos;

            if ( filtered_events )
            {
                auto i = std::lower_bound( m_eventlist.filtered_events.begin(), m_eventlist.filtered_events.end(), m_eventlist.goto_eventid );

                pos = i - m_eventlist.filtered_events.begin();
            }
            else
            {
                pos = m_eventlist.goto_eventid;
            }

            pos = std::min< uint32_t >( pos, event_count - 1 );

            // Only scroll if our goto event isn't visible.
            if ( ( uint32_t )m_eventlist.goto_eventid <= m_eventlist.start_eventid )
            {
                float scrolly = std::max< int >( 0, pos ) * lineh;

                ImGui::SetScrollY( scrolly );
            }
            else if ( ( uint32_t )m_eventlist.goto_eventid + 1 >= m_eventlist.end_eventid )
            {
                float scrolly = std::max< int >( 0, pos - visible_rows + 1 ) * lineh;

                ImGui::SetScrollY( scrolly );
            }

            // Select the event also
            m_eventlist.selected_eventid = m_eventlist.goto_eventid;

            m_eventlist.do_gotoevent = false;
        }

        float scrolly = ImGui::GetScrollY();
        uint32_t start_idx = std::max< float >( scrolly / lineh, 1.0f ) - 1;
        uint32_t end_idx = std::min< uint32_t >( start_idx + 2 + visible_rows, event_count );

        // Draw columns
        imgui_begin_columns( "event_list", { "Id", "Time Stamp", "Task", "Event", "Duration", "Info" },
                             &inifile, &m_eventlist.columns_resized );
        {
            bool popup_shown = false;
            int64_t ts_marker_diff = 0;

            // Reset our hovered event id
            m_eventlist.hovered_eventid = INVALID_ID;

            // Move cursor position down to where we've scrolled.
            if ( start_idx > 0 )
                ImGui::SetCursorPosY( ImGui::GetCursorPosY() + lineh * ( start_idx - 1 ) );

            if ( filtered_events )
            {
                m_eventlist.start_eventid = m_eventlist.filtered_events[ start_idx ];
                m_eventlist.end_eventid = m_eventlist.filtered_events[ end_idx - 1 ];
            }
            else
            {
                m_eventlist.start_eventid = start_idx;
                m_eventlist.end_eventid = end_idx;
            }

            // Loop through and draw events
            for ( uint32_t i = start_idx; i < end_idx; i++ )
            {
                trace_event_t &event = filtered_events ?
                            m_trace_events.m_events[ m_eventlist.filtered_events[ i ] ] :
                            m_trace_events.m_events[ i ];
                bool selected = ( m_eventlist.selected_eventid == event.id );
                ImVec2 cursorpos = ImGui::GetCursorScreenPos();

                ImGui::PushID( i );

                if ( event.is_vblank() )
                {
                    // If this is a vblank event, draw the text in blue or red vblank colors
                    ImColor col = Clrs::get( ( event.crtc > 0 ) ? col_VBlank1 : col_VBlank0 );

                    ImGui::PushStyleColor( ImGuiCol_Text, col );
                }

                // If this event is in the highlighted list, give it a bit of a colored background
                bool highlight = !selected && std::binary_search(
                            m_eventlist.highlight_ids.begin(), m_eventlist.highlight_ids.end(), event.id );
                if ( highlight )
                    ImGui::PushStyleColor( ImGuiCol_Header, Clrs::getv4( col_EventListHov ) );

                // column 0: event id
                {
                    if ( ImGui::Selectable( std::to_string( event.id ).c_str(),
                                            highlight || selected, ImGuiSelectableFlags_SpanAllColumns ) )
                    {
                        m_eventlist.selected_eventid = event.id;
                    }

                    // Columns bug workaround: selectable with SpanAllColumns & overlaid button does not work.
                    // https://github.com/ocornut/imgui/issues/684
                    ImGui::SetItemAllowOverlap();

                    // Handle popup menu / tooltip
                    popup_shown |= events_list_handle_mouse( event, i );

                    ImGui::NextColumn();
                }

                // column 1: time stamp
                {
                    std::string ts_str = ts_to_timestr( event.ts, m_eventlist.tsoffset ) + "ms";

                    if ( event.id > 0 )
                    {
                        // Add time delta from previous event
                        const trace_event_t &event0 = get_event( event.id - 1 );
                        ts_str += " (+" + ts_to_timestr( event.ts - event0.ts, 0, 4 ) + ")";
                    }

                    ImGui::Text( "%s", ts_str.c_str() );
                    ImGui::NextColumn();
                }

                // column 2: comm
                {
                    ImGui::Text( "%s (%u)", event.comm, event.cpu );
                    ImGui::NextColumn();
                }

                // column 3: event name
                {
                    ImGui::Text( "%s", event.name );
                    ImGui::NextColumn();
                }

                // column 4: duration
                {
                    if ( event.duration )
                        ImGui::Text( "%sms", ts_to_timestr( event.duration, 0, 4 ).c_str() );
                    ImGui::NextColumn();
                }

                // column 5: event fields
                {
                    if ( event.is_ftrace_print() )
                    {
                        ImGui::TextColored( Clrs::getv4( col_FtracePrintText ), "%s", get_event_field_val( event.fields, "buf" ) );
                    }
                    else
                    {
                        std::string fieldstr = get_event_fields_str( event, "=", ' ' );

                        ImGui::Text( "%s", fieldstr.c_str() );
                    }
                    ImGui::NextColumn();
                }

                // Draw time stamp marker diff line if we're right below ts_marker_mouse
                {
                    int64_t ts_diff = event.ts - m_graph.ts_marker_mouse;
                    bool do_draw_ts_line = ( ts_marker_diff < 0 ) && ( ts_diff > 0 );

                    if ( do_draw_ts_line )
                        draw_ts_line( cursorpos );

                    ts_marker_diff = ts_diff;
                }

                ImGui::PopStyleColor( event.is_vblank() + highlight );
                ImGui::PopID();
            }

            if ( !popup_shown )
            {
                // When we modify a filter via the context menu, it can hide the item
                //  we right clicked on which means events_list_render_popupmenu() won't get
                //  called. Check for that case here.
                m_eventlist.popup_eventid = INVALID_ID;
            }
        }
        if ( ImGui::EndColumns() )
            m_eventlist.columns_resized = true;

        ImGui::EndChild();
    }
}

void TraceLoader::render_menu_options()
{
    {
        ImGui::TextColored( ImVec4( 1, 1, 0, 1 ), "%s", "Windows" );
        ImGui::Indent();

        if ( ImGui::MenuItem( "Gpuvis Console" ) )
        {
            ImGui::SetWindowFocus( "Gpuvis Console" );
            m_show_gpuvis_console = true;
        }

        if ( ImGui::MenuItem( "Font Options" ) )
        {
            ImGui::SetWindowFocus( "Font Options" );
            m_show_font_window = true;
        }

        if ( ImGui::MenuItem( "Color Configuration" ) )
        {
            ImGui::SetWindowFocus( "Color Configuration" );
            m_show_color_picker = true;
        }

        ImGui::Separator();

        if ( ImGui::MenuItem( "ImGui Style Editor" ) )
        {
            ImGui::SetWindowFocus( "Style Editor" );
            m_show_imgui_style_editor = true;
        }

        if ( ImGui::MenuItem( "ImGui Metrics" ) )
        {
            ImGui::SetWindowFocus( "ImGui Metrics" );
            m_show_imgui_metrics_editor = true;
        }

        if ( ImGui::MenuItem( "ImGui Test Window" ) )
        {
            ImGui::SetWindowFocus( "ImGui Demo" );
            m_show_imgui_test_window = true;
        }

        ImGui::Unindent();
    }

    ImGui::Separator();

    ImGui::TextColored( ImVec4( 1, 1, 0, 1 ), "%s", "Gpuvis Settings" );
    ImGui::Indent();

    for ( size_t i = 0; i < m_options.size(); i++ )
    {
        option_t &opt = m_options[ i ];

        if ( opt.hidden )
            continue;

        if ( ( i >= OPT_RenderCrtc0 ) && ( i <= OPT_RenderCrtc9 ) )
        {
            if ( i - OPT_RenderCrtc0 > m_crtc_max )
                continue;
        }

        imgui_opt( i );
    }

    ImGui::Unindent();
}

void TraceLoader::render_font_options()
{
    static const char lorem_str[] =
        "Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do"
        "eiusmod tempor incididunt ut labore et dolore magna aliqua. Ut enim ad minim"
        "veniam, quis nostrud exercitation ullamco laboris nisi ut aliquip ex ea commodo"
        "consequat. Duis aute irure dolor in reprehenderit in voluptate velit esse"
        "cillum dolore eu fugiat nulla pariatur. Excepteur sint occaecat cupidatat non"
        "proident, sunt in culpa qui officia deserunt mollit anim id est laborum.";

    ImGui::Indent();
    ImGui::PushID( "font_options" );

    {
        bool changed = false;

#ifdef USE_FREETYPE
        changed |= imgui_opt( OPT_UseFreetype );
#endif

        changed |= imgui_opt( OPT_Scale );

        if ( ImGui::Button( "Reset to Defaults" ) )
        {
            m_font_main.m_reset = true;
            m_font_small.m_reset = true;
            changed = true;
        }

        if ( changed )
        {
            // Ping font change so this stuff will reload in main loop.
            m_font_main.m_changed = true;
        }
    }

    if ( ImGui::TreeNodeEx( "Main Font", ImGuiTreeNodeFlags_DefaultOpen ) )
    {
        const char *font_name = m_font_main.m_name.c_str();

        ImGui::TextWrapped( "%s: %s", multi_text_color::bright_text.m_str( font_name ).c_str(), lorem_str );

        m_font_main.render_font_options( !!m_options[ OPT_UseFreetype ].val );
        ImGui::TreePop();
    }

    if ( ImGui::TreeNodeEx( "Small Font", ImGuiTreeNodeFlags_DefaultOpen ) )
    {
        const char *font_name = m_font_small.m_name.c_str();

        ImGui::BeginChild( "small_font", ImVec2( 0, ImGui::GetTextLineHeightWithSpacing() * 4 ) );

        imgui_push_smallfont();
        ImGui::TextWrapped( "%s: %s", multi_text_color::bright_text.m_str( font_name ).c_str(), lorem_str );
        imgui_pop_smallfont();

        ImGui::EndChild();

        m_font_small.render_font_options( !!m_options[ OPT_UseFreetype ].val );

        ImGui::TreePop();
    }

    ImFontAtlas* atlas = ImGui::GetIO().Fonts;
    if (ImGui::TreeNode( "Font atlas texture", "Atlas texture (%dx%d pixels)", atlas->TexWidth, atlas->TexHeight ) )
    {
        ImGui::Image( atlas->TexID,
                      ImVec2( (float )atlas->TexWidth, ( float )atlas->TexHeight),
                      ImVec2( 0, 0 ), ImVec2( 1, 1 ),
                      ImColor( 255, 255, 255, 255 ), ImColor( 255, 255, 255, 128 ) );
        ImGui::TreePop();
    }

    ImGui::PopID();
    ImGui::Unindent();
}

void TraceLoader::render_color_picker()
{
    bool changed = imgui_opt( OPT_DarkTheme );

    if ( ImGui::Button( "Reset to Defaults" ) )
    {
        for ( colors_t i = 0; i < col_Max; i++ )
            Clrs::reset( i );

        m_options[ OPT_DarkTheme ].val = true;
        changed = true;
    }

    ImGui::Separator();

    if ( ImGui::BeginColumns( "color_picker", 2, 0 ) )
        ImGui::SetColumnWidth( 0, imgui_scale( 250.0f ) );

    /*
     * Column 1: draw our graph items and their colors
     */
    {
        ImGui::BeginChild( "color_list" );

        float w = imgui_scale( 32.0f );
        float text_h = ImGui::GetTextLineHeight();

        for ( colors_t i = 0; i < col_Max; i++ )
        {
            bool selected = ( i == m_selected_color );
            ImVec2 pos = ImGui::GetCursorScreenPos();
            ImU32 col = Clrs::get( i );
            const char *name = Clrs::name( i );

            ImGui::GetWindowDrawList()->AddRectFilled( pos, ImVec2( pos.x + w, pos.y + text_h ), col );

            ImGui::Indent( imgui_scale( 40.0f ) );
            if ( ImGui::Selectable( name, selected, 0 ) )
                m_selected_color = i;
            ImGui::Unindent( imgui_scale( 40.0f ) );
        }

        ImGui::EndChild();
    }
    ImGui::NextColumn();

    /*
     * Column 2: Draw our color picker
     */
    {
        ImU32 color;

        const char *name = Clrs::name( m_selected_color );

        imgui_text_bg( name, ImGui::GetColorVec4( ImGuiCol_Header ) );

        if ( m_selected_color == col_ThemeAlpha ||
             m_selected_color == col_ColorLabelSat ||
             m_selected_color == col_ColorLabelAlpha )
        {
            float val = Clrs::getalpha( m_selected_color );

            ImGui::PushItemWidth( imgui_scale( 125.0f ) );

            if ( ImGui::SliderFloat( "##alpha_val", &val, 0.0f, 1.0f, "%.02f" ) )
            {
                Clrs::set( m_selected_color, ImColor( val, val, val, val ) );

                if ( m_selected_color == col_ColorLabelSat ||
                     m_selected_color == col_ColorLabelAlpha )
                {
                    for ( TraceEvents *trace_event : m_trace_events_list )
                        trace_event->m_rect_size_max_x = -1.0f;
                }

                changed = true;
            }

            ImGui::PopItemWidth();

            if ( ImGui::Button( "Reset to Default" ) )
            {
                Clrs::reset( m_selected_color );
                changed = true;
            }
        }
        else
        {
            ImGui::NewLine();

            if ( m_colorpicker.render( &color ) )
            {
                Clrs::set( m_selected_color, color );

                if ( m_selected_color == col_FtracePrintText || m_selected_color == col_BrightText )
                    multi_text_color::update_colors();

                if ( ( m_selected_color >= col_ImGui_Text ) && ( m_selected_color <= col_ImGui_ModalWindowDarkening ) )
                    changed = true;
            }

            ImGui::NewLine();
            if ( ImGui::Button( "Reset to Default" ) )
            {
                Clrs::reset( m_selected_color );
                changed = true;
            }
        }
    }
    ImGui::NextColumn();

    ImGui::EndColumns();

    if ( changed )
    {
        imgui_set_custom_style( !!get_opt( OPT_DarkTheme ), Clrs::getalpha( col_ThemeAlpha ) );

        multi_text_color::update_colors();
    }
}

void TraceLoader::render_log()
{
    ImGui::Text( "Log Filter:" );
    ImGui::SameLine();
    ImGui::PushStyleVar( ImGuiStyleVar_FramePadding, ImVec2( 0, 0 ) );
    m_filter.Draw( "##log-filter", 180 );
    ImGui::PopStyleVar();

    ImGui::SameLine();
    if ( ImGui::SmallButton( "Clear" ) )
        logf_clear();

    ImGui::SameLine();
    if ( ImGui::SmallButton( "Scroll to bottom" ) )
        m_log_size = ( size_t )-1;

    ImGui::Separator();

    {
        ImGui::BeginChild( "ScrollingRegion",
                           ImVec2( 0, -ImGui::GetItemsLineHeightWithSpacing() ),
                           false, ImGuiWindowFlags_HorizontalScrollbar );

        // Log popup menu
        if ( ImGui::BeginPopupContextWindow() )
        {
            if ( ImGui::Selectable( "Clear" ) )
                logf_clear();
            ImGui::EndPopup();
        }

        // Display every line as a separate entry so we can change their color or add custom widgets. If you only want raw text you can use ImGui::TextUnformatted(log.begin(), log.end());
        // NB- if you have thousands of entries this approach may be too inefficient and may require user-side clipping to only process visible items.
        // You can seek and display only the lines that are visible using the ImGuiListClipper helper, if your elements are evenly spaced and you have cheap random access to the elements.
        // To use the clipper we could replace the 'for (int i = 0; i < Items.Size; i++)' loop with:
        //     ImGuiListClipper clipper(Items.Size);
        //     while (clipper.Step())
        //         for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++)
        // However take note that you can not use this code as is if a filter is active because it breaks the 'cheap random-access' property. We would need random-access on the post-filtered list.
        // A typical application wanting coarse clipping and filtering may want to pre-compute an array of indices that passed the filtering test, recomputing this array when user changes the filter,
        // and appending newly elements as they are inserted. This is left as a task to the user until we can manage to improve this example code!
        // If your items are of variable size you may want to implement code similar to what ImGuiListClipper does. Or split your data into fixed height items to allow random-seeking into your list.

        // Tighten spacing
        ImGui::PushStyleVar( ImGuiStyleVar_ItemSpacing, ImVec2( 4, 1 ) );

        const std::vector< char * > &log = logf_get();
        for ( const char *item : log )
        {
            if ( !m_filter.PassFilter( item ) )
                continue;

            ImVec4 col = ImVec4( 1, 1, 1, 1 );

            if ( !strncasecmp( item, "[error]", 7 ) )
                col = ImColor( 1.0f, 0.4f, 0.4f, 1.0f );
            else if ( strncmp( item, "# ", 2 ) == 0 )
                col = ImColor( 1.0f, 0.78f, 0.58f, 1.0f );

            ImGui::PushStyleColor( ImGuiCol_Text, col );
            ImGui::TextUnformatted( item );
            ImGui::PopStyleColor();
        }

        if ( m_log_size != log.size() )
        {
            ImGui::SetScrollHere();

            m_log_size = log.size();
        }

        ImGui::PopStyleVar();
        ImGui::EndChild();
    }
}

void TraceLoader::render_console()
{
    if ( !ImGui::Begin( "Gpuvis Console", &m_show_gpuvis_console, ImGuiWindowFlags_MenuBar ) )
    {
        ImGui::End();
        return;
    }

    render_menu();

    ImGui::Text( "%.2f ms/frame (%.1f FPS)",
                 1000.0f / ImGui::GetIO().Framerate, ImGui::GetIO().Framerate );

    render_log();

    ImGui::End();
}

void TraceLoader::render_menu()
{
    if ( !ImGui::BeginMenuBar() )
        return;

    if ( ImGui::BeginMenu( "File" ) )
    {
#if defined( NOC_FILE_DIALOG_IMPLEMENTATION )
        if ( ImGui::MenuItem( "Open Trace File..." ) )
        {
            const char *file = noc_file_dialog_open( NOC_FILE_DIALOG_OPEN,
                "trace-cmd files (*.dat)\0*.dat\0", NULL, "trace.dat" );

            if ( file && file[ 0 ] )
                m_inputfiles.push_back( file );
        }
#endif

        if ( ImGui::MenuItem( "Quit" ) )
        {
            SDL_Event event;

            event.type = SDL_QUIT;
            SDL_PushEvent( &event );
        }

        ImGui::EndMenu();
    }

    if ( ImGui::BeginMenu( "Options") )
    {
        render_menu_options();
        ImGui::EndMenu();
    }

    ImGui::EndMenuBar();
}

void TraceLoader::parse_cmdline( int argc, char **argv )
{
    static struct option long_opts[] =
    {
        { "scale", ya_required_argument, 0, 0 },
        { 0, 0, 0, 0 }
    };

    int c;
    int opt_ind = 0;
    while ( ( c = ya_getopt_long( argc, argv, "i:",
                               long_opts, &opt_ind ) ) != -1 )
    {
        switch ( c )
        {
        case 0:
            if ( !strcasecmp( "scale", long_opts[ opt_ind ].name ) )
                m_options[ OPT_Scale ].valf = atof( ya_optarg );
            break;
        case 'i':
            m_inputfiles.clear();
            m_inputfiles.push_back( ya_optarg );
            break;

        default:
            break;
        }
    }

    for ( ; ya_optind < argc; ya_optind++ )
    {
        m_inputfiles.clear();
        m_inputfiles.push_back( argv[ ya_optind ] );
    }
}

#if SDL_VERSIONNUM( SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_PATCHLEVEL ) < SDL_VERSIONNUM( 2, 0, 5 )
int SDL_GetWindowBordersSize( SDL_Window *window, int *top, int *left, int *bottom, int *right )
{
    *top = 0;
    *left = 0;
    *bottom = 0;
    *right = 0;

    return -1;
}
#endif

static void sdl_setwindow_icon( SDL_Window *window )
{
#include "gpuvis_icon.c"

    SDL_Surface *surface = SDL_CreateRGBSurfaceFrom(
                s_icon.pixel_data,
                s_icon.width,
                s_icon.height,
                s_icon.bytes_per_pixel * 8,
                s_icon.width * s_icon.bytes_per_pixel,
                IM_COL32( 0xff, 0, 0, 0 ),
                IM_COL32( 0, 0xff, 0, 0 ),
                IM_COL32( 0, 0, 0xff, 0 ),
                IM_COL32( 0, 0, 0, 0xff ) );

    SDL_SetWindowIcon( window, surface );
}

int main( int argc, char **argv )
{
    // Initialize SDL
    if ( SDL_Init( SDL_INIT_VIDEO | SDL_INIT_TIMER ) )
    {
        fprintf( stderr, "Error. SDL_Init failed: %s\n", SDL_GetError() );
        return -1;
    }

    // Initialize logging system
    logf_init();

    TraceLoader loader;
    SDL_Window *window = NULL;
    SDL_Cursor *cursor_sizens = SDL_CreateSystemCursor( SDL_SYSTEM_CURSOR_SIZENS );
    SDL_Cursor *cursor_default = SDL_GetDefaultCursor();

    loader.init( argc, argv );

    // Setup window
    SDL_GL_SetAttribute( SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG );
    SDL_GL_SetAttribute( SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE );
    SDL_GL_SetAttribute( SDL_GL_DOUBLEBUFFER, 1 );
    SDL_GL_SetAttribute( SDL_GL_DEPTH_SIZE, 24 );
    SDL_GL_SetAttribute( SDL_GL_STENCIL_SIZE, 8 );
    SDL_GL_SetAttribute( SDL_GL_CONTEXT_MAJOR_VERSION, 3 );
    SDL_GL_SetAttribute( SDL_GL_CONTEXT_MINOR_VERSION, 2 );

    int x, y, w, h;
    loader.get_window_pos( x, y, w, h );
    window = SDL_CreateWindow( "GPUVis", x, y, w, h, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE );
    sdl_setwindow_icon( window );

    SDL_GLContext glcontext = SDL_GL_CreateContext( window );

    gl3wInit();

    // Setup ImGui binding
    ImGui_ImplSdlGL3_Init( window );

    // 1 for updates synchronized with the vertical retrace
    SDL_GL_SetSwapInterval( 1 );

    // Setup imgui default text color
    multi_text_color::update_colors();

    // Load our fonts
    loader.load_fonts();

    // Main loop
    bool done = false;
    ImGuiMouseCursor mouse_cursor = ImGuiMouseCursor_Arrow;
    while ( !done )
    {
        SDL_Event event;

        if ( mouse_cursor != ImGui::GetMouseCursor() )
        {
            mouse_cursor = ImGui::GetMouseCursor();

            SDL_SetCursor( ( mouse_cursor == ImGuiMouseCursor_ResizeNS ) ?
                               cursor_sizens : cursor_default );
        }

        while ( SDL_PollEvent( &event ) )
        {
            ImGui_ImplSdlGL3_ProcessEvent( &event );

            if ( event.type == SDL_QUIT )
                done = true;
        }
        ImGui_ImplSdlGL3_NewFrame( window, &loader.m_options[ OPT_UseFreetype ].val );

        // Check for logf() calls from background threads.
        logf_update();

        // Render trace windows
        loader.render();

        {
            // ImGui Rendering
            const ImVec4 color = ( ImColor )Clrs::get( col_ClearColor );
            const ImVec2 &size = ImGui::GetIO().DisplaySize;

            glViewport( 0, 0, ( int )size.x, ( int )size.y );
            glClearColor( color.x, color.y, color.z, color.w );
            glClear( GL_COLOR_BUFFER_BIT );

            ImGui::Render();

            SDL_GL_SwapWindow( window );
        }

        if ( loader.m_quit )
            break;

        if ( !loader.m_inputfiles.empty() && !loader.is_loading() )
        {
            const char *filename = loader.m_inputfiles[ 0 ].c_str();

            loader.load_file( filename );

            loader.m_inputfiles.erase( loader.m_inputfiles.begin() );
        }

        if ( ( loader.m_font_main.m_changed || loader.m_font_small.m_changed ) &&
             !ImGui::IsMouseDown( 0 ) )
        {
            imgui_set_scale( loader.get_optf( OPT_Scale ) );

            ImGui_ImplSdlGL3_InvalidateDeviceObjects();
            loader.load_fonts();
        }
    }

    {
        // Write main window position / size to ini file.
        int top, left, bottom, right;

        SDL_GetWindowBordersSize( window, &top, &left, &bottom, &right );
        SDL_GetWindowPosition( window, &x, &y );
        SDL_GetWindowSize( window, &w, &h );

        loader.save_window_pos( x - left, y - top, w, h );
    }

    // Shut down trace loader
    loader.shutdown();

    logf_clear();

    // Cleanup
    logf_shutdown();

    ImGui_ImplSdlGL3_Shutdown();

    SDL_FreeCursor( cursor_sizens );

    SDL_GL_DeleteContext( glcontext );
    SDL_DestroyWindow( window );
    SDL_Quit();
    return 0;
}
