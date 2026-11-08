/**
 * @file
 * @brief A help screen from a .GFX file, e.g. HELP\HITEM.GFX.
 *
 * The file's #D number picks the popup -- 14 + n -- and the page is laid
 * out inside it, every position counted from the popup's corner:
 *
 *   #H          font 1, colour 15        #T      font 2, colour 15
 *   #Fnnn       font 2, colour nnn       #E      the end
 *   #C(x,y)|t|  centred on x             #L(x,y)|t|  from x
 *   #R(x,y)|t|  ending at x
 *   #G(x,y,w,h)nnn(dx,dy)  the (x, y) w x h of bitmap nnn, at (dx, dy)
 *
 * A key or a click puts it away. The control panel's "?" shows HMOUSE and
 * then HKEYS in popup 4, (120, 50) 400x360.
 */
#include <regex>

#include "ui/dialogs.hpp"
#include "util/util.hpp"

namespace help {

const kit::Rect kPopuP4{120, 50, 400, 360};

namespace {
/// The popups a page's #D picks: 0 (or none) popup 14, 1 popup 15.
const kit::Rect kPopups[2] = {
    {256, 40, 352, 340},  // popup 14
    {32, 40, 352, 340},   // popup 15
};

/** @brief One instruction of a help page: a font change, a line of text or a piece of bitmap. */
struct Step {
    int font = 0;          ///< A font change to this font when not 0.
    int colour = 15;       ///< The text colour that comes with a font change.
    bool is_text = false;  ///< A line of text.
    std::string text;      ///< The text, for a line of text.
    char align = 'L';      ///< 'C' centred on x, 'L' from x, 'R' ending at x.
    int x = 0;             ///< Where the text or the bitmap piece goes, from the popup's corner.
    int y = 0;             ///< Where the text or the bitmap piece goes, from the popup's corner.
    int bitmap = -1;       ///< The bitmap (FILE.DAT art) a piece is cut from, or -1.
    int source_x = 0;      ///< The piece's left edge in the bitmap.
    int source_y = 0;      ///< The piece's top edge in the bitmap.
    int width = 0;         ///< The piece's width.
    int height = 0;        ///< The piece's height.
};

/**
 * @brief Reads a .GFX help page into steps.
 * @param text The file's contents.
 * @param steps Gets the page's steps appended, up to #E.
 * @return The page's #D number, which picks the popup (0 when there is none).
 */
int Parse(const std::string& text, std::vector<Step>& steps) {
    int page = 0;
    static const std::regex kLineRe("^#([A-Z])(.*)$");
    static const std::regex kNumRe("^(\\d+)");
    static const std::regex kTextRe("^\\((\\d+),(\\d+)\\)\\|(.*?)\\|");
    static const std::regex kGfxRe("^\\((\\d+),(\\d+),(\\d+),(\\d+)\\)(\\d+)\\((\\d+),(\\d+)\\)");
    for (auto& line : w2::SplitLines(text)) {
        std::smatch line_match;
        if (!std::regex_search(line, line_match, kLineRe)) continue;
        char code = line_match[1].str()[0];
        std::string rest = line_match[2];
        std::smatch args;
        if (code == 'D') {
            page = std::regex_search(rest, args, kNumRe) ? std::stoi(args[1]) : 0;
        } else if (code == 'H') {
            Step step;
            step.font = 1;
            step.colour = 15;
            steps.push_back(step);
        } else if (code == 'T') {
            Step step;
            step.font = 2;
            step.colour = 15;
            steps.push_back(step);
        } else if (code == 'F') {
            Step step;
            step.font = 2;
            step.colour = std::regex_search(rest, args, kNumRe) ? std::stoi(args[1]) : 15;
            steps.push_back(step);
        } else if (code == 'C' || code == 'L' || code == 'R') {
            if (std::regex_search(rest, args, kTextRe)) {
                Step step;
                step.is_text = true;
                step.text = args[3];
                step.align = code;
                step.x = std::stoi(args[1]);
                step.y = std::stoi(args[2]);
                steps.push_back(step);
            }
        } else if (code == 'G') {
            if (std::regex_search(rest, args, kGfxRe)) {
                Step step;
                step.bitmap = std::stoi(args[5]);
                step.source_x = std::stoi(args[1]);
                step.source_y = std::stoi(args[2]);
                step.width = std::stoi(args[3]);
                step.height = std::stoi(args[4]);
                step.x = std::stoi(args[6]);
                step.y = std::stoi(args[7]);
                steps.push_back(step);
            }
        } else if (code == 'E') {
            break;
        }
    }
    return page;
}

/** @brief A help page on the screen; any key or click puts it away. */
struct Page : kit::Modal {
    std::vector<Step> steps;  ///< The page, in drawing order.
    kit::Rect rect;           ///< The popup it is drawn in.
    kit::Done after;          ///< Runs once it has closed.

    /** @brief Closes, then runs `after`. */
    void Close() {
        auto after_copy = after;
        kit::Pop(this);
        if (after_copy) after_copy();
    }

    /** @brief Draws the popup and runs the steps, font changes holding until the next. */
    void Draw() override {
        kit::Popup(rect);
        const Font* font = &kit::GetFont(2);
        int colour = 15;
        for (const Step& step : steps) {
            if (step.font) {
                font = &kit::GetFont(step.font);
                colour = step.colour;
            } else if (step.is_text) {
                const Font& coloured = font->Colours(colour, 0);
                gfx::SetColor(1, 1, 1);
                if (step.align == 'C')
                    kit::Centred(coloured, step.text, rect.x + step.x, rect.y + step.y);
                else if (step.align == 'R')
                    kit::Right(coloured, step.text, rect.x + step.x, rect.y + step.y);
                else
                    coloured.Draw(step.text, rect.x + step.x, rect.y + step.y);
            } else if (step.bitmap >= 0) {
                if (auto art = g_app.screen->ArtFor(step.bitmap)) {
                    gfx::SetColor(1, 1, 1);
                    gfx::Draw(art->image,
                              gfx::NewQuad(step.source_x, step.source_y, step.width, step.height),
                              rect.x + step.x, rect.y + step.y);
                }
            }
        }
    }
    void Mousepressed(int, int, int) override { Close(); }
    void Keypressed(const std::string&) override { Close(); }
};
}  // namespace

/**
 * @brief Shows a help file in a popup.
 * @param name The file under the data directory, DOS-style ("HELP\HITEM.GFX").
 * @param after Runs once the page has been put away.
 * @param rect The popup to use; when null, the one the file's #D picks.
 * @return False when there is no such file.
 */
bool Open(const std::string& name, kit::Done after, const kit::Rect* rect) {
    std::string path = name;
    for (auto& character : path)
        if (character == '\\') character = '/';
    auto text = w2::ReadFile(g_app.data_dir / path);
    if (!text.ok()) return false;
    auto dialog = std::make_shared<Page>();
    int page = Parse(*text, dialog->steps);
    dialog->rect = rect ? *rect : (page == 1 ? kPopups[1] : kPopups[0]);
    dialog->after = after;
    kit::Push(dialog);
    return true;
}

}  // namespace help
