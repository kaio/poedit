 
/*

    poedit, a wxWindows i18n catalogs editor

    ---------------
      catalog.cpp
    
      Translations catalog
    
      (c) Vaclav Slavik, 1999-2004

*/


#include <wx/wxprec.h>

#include <stdio.h>
#include <wx/utils.h>
#include <wx/tokenzr.h>
#include <wx/log.h>
#include <wx/intl.h>
#include <wx/datetime.h>
#include <wx/config.h>
#include <wx/textfile.h>
#include <wx/strconv.h>
#include <wx/msgdlg.h>

#include "catalog.h"
#include "digger.h"
#include "gexecute.h"
#include "progressinfo.h"
#include "summarydlg.h"


#include "wx/arrimpl.cpp"
WX_DEFINE_OBJARRAY(CatalogDataArray)

    
// ----------------------------------------------------------------------
// Textfile processing utilities:
// ----------------------------------------------------------------------


// Read one line from file, remove all \r and \n characters, ignore empty lines
static wxString ReadTextLine(wxTextFile* f, wxMBConv *conv)
{
    wxString s;
    
    while (s.IsEmpty())
    {
        if (f->Eof()) return wxEmptyString;
        s = f->GetNextLine();
    }
#if !wxUSE_UNICODE
    if (conv)
        s = wxString(s.wc_str(*conv), wxConvUTF8);
#endif
    return s;
}


// If input begins with pattern, fill output with end of input (without
// pattern; strips trailing spaces) and return true.  Return false otherwise
// and don't touch output
static bool ReadParam(const wxString& input, const wxString& pattern, wxString& output)
{
    if (input.Length() < pattern.Length()) return false;
    
    for (unsigned i = 0; i < pattern.Length(); i++)
        if (input[i] != pattern[i]) return false;
    output = input.Mid(pattern.Length()).Strip(wxString::trailing);
    return true;
}
        
            

// ----------------------------------------------------------------------
// Catalog::HeaderData
// ----------------------------------------------------------------------

void Catalog::HeaderData::FromString(const wxString& str)
{
    wxString hdr(str);
    hdr.Replace(_T("\\n"), _T("\n"));
    wxStringTokenizer tkn(hdr, _T("\n"));
    wxString ln;

    m_entries.clear();

    while (tkn.HasMoreTokens())
    {
        ln = tkn.GetNextToken();
        size_t pos = ln.find(_T(": "));
        if (pos == wxString::npos)
        {
            wxLogError(_("Malformed header: '%s'"), ln.c_str());
        }
        else
        {
            Entry en;
            en.Key = ln.substr(0, pos);
            en.Value = ln.substr(pos + 2);
            m_entries.push_back(en);
        }
    }

    ParseDict();
}

wxString Catalog::HeaderData::ToString(const wxString& line_delim)
{
    UpdateDict();

    wxString hdr;
    for (std::vector<Entry>::const_iterator i = m_entries.begin();
         i != m_entries.end(); ++i)
    {
        hdr << i->Key << _T(": ") << i->Value << _T("\\n") << line_delim;
    }
    return hdr;
}
            
void Catalog::HeaderData::UpdateDict()
{
    SetHeader(_T("Project-Id-Version"), Project);
    SetHeader(_T("POT-Creation-Date"), CreationDate);
    SetHeader(_T("PO-Revision-Date"), RevisionDate);
    
    if (TranslatorEmail.empty())
        SetHeader(_T("Last-Translator"), Translator);
    else
        SetHeader(_T("Last-Translator"),
                  Translator + _T(" <") + TranslatorEmail + _T(">"));
    
    if (TeamEmail.empty())
        SetHeader(_T("Language-Team"), Team);
    else
        SetHeader(_T("Language-Team"),
                  Team + _T(" <") + TeamEmail + _T(">"));
    
    SetHeader(_T("MIME-Version"), _T("1.0"));
    SetHeader(_T("Content-Type"), _T("text/plain; charset=") + Charset);
    SetHeader(_T("Content-Transfer-Encoding"), _T("8bit"));
}

void Catalog::HeaderData::ParseDict()
{
    wxString dummy;
    
    Project = GetHeader(_T("Project-Id-Version"));
    CreationDate = GetHeader(_T("POT-Creation-Date"));
    RevisionDate = GetHeader(_T("PO-Revision-Date"));
           
    dummy = GetHeader(_T("Last-Translator"));
    if (!dummy.empty())
    {
        wxStringTokenizer tkn(dummy, _T("<>"));
        if (tkn.CountTokens() != 2)
        {
            Translator = dummy;
            TranslatorEmail = wxEmptyString;
        }
        else
        {
            Translator = tkn.GetNextToken().Strip(wxString::trailing);
            TranslatorEmail = tkn.GetNextToken();
        }
    }

    dummy = GetHeader(_T("Language-Team"));
    if (!dummy.empty())
    {
        wxStringTokenizer tkn(dummy, _T("<>"));
        if (tkn.CountTokens() != 2) 
        {
            Team = dummy;
            TeamEmail = wxEmptyString;
        }
        else
        {
            Team = tkn.GetNextToken().Strip(wxString::trailing);
            TeamEmail = tkn.GetNextToken();
        }
    }
        
    wxString ctype = GetHeader(_T("Content-Type"));
    if (!ReadParam(ctype, _T("text/plain; charset="), Charset))
        Charset = _T("iso-8859-1");
}

wxString Catalog::HeaderData::GetHeader(const wxString& key) const
{
    const Entry *e = Find(key);
    if (e)
        return e->Value;
    else
        return wxEmptyString;
}

bool Catalog::HeaderData::HasHeader(const wxString& key) const
{
    return Find(key) != NULL;
}

void Catalog::HeaderData::SetHeader(const wxString& key, const wxString& value)
{
    Entry *e = (Entry*) Find(key);
    if (e)
    {
        e->Value = value;
    }
    else
    {
        Entry en;
        en.Key = key;
        en.Value = value;
        m_entries.push_back(en);
    }
}

const Catalog::HeaderData::Entry *
Catalog::HeaderData::Find(const wxString& key) const
{
    size_t size = m_entries.size();
    for (size_t i = 0; i < size; i++)
    {
        if (m_entries[i].Key == key)
            return &(m_entries[i]);
    }
    return NULL;
}


// ----------------------------------------------------------------------
// Parsers
// ----------------------------------------------------------------------
        
void CatalogParser::Parse()
{
    if (m_textFile->GetLineCount() == 0) return;

    wxString line, dummy;
    wxString mflags, mstr, mtrans, mcomment;
    wxArrayString mrefs,mautocomments;
    unsigned mlinenum;
    
    line = m_textFile->GetFirstLine();
    if (line.IsEmpty()) line = ReadTextLine(m_textFile, m_conv);

    while (!line.IsEmpty())
    {
        // ignore empty special tags:
        while (line == _T("#,") || line == _T("#:") || line == _T("#."))
            line = ReadTextLine(m_textFile, m_conv);
        
	// auto comments:
	if (ReadParam(line, _T("#. "), dummy))
        {
            mautocomments.Add(dummy);
            line = ReadTextLine(m_textFile, m_conv);
        }
 
        // flags:
	// Can't we have more than one flag, now only the last is kept ...
        if (ReadParam(line, _T("#, "), dummy))
        {
            mflags = _T("#, ") + dummy;
            line = ReadTextLine(m_textFile, m_conv);
        }
        
        // references:
        if (ReadParam(line, _T("#: "), dummy))
        {
            wxStringTokenizer tkn(dummy, _T("\t\r\n "));
            while (tkn.HasMoreTokens())
                mrefs.Add(tkn.GetNextToken());
            line = ReadTextLine(m_textFile, m_conv);
        }
        
        // msgid:
        else if (ReadParam(line, _T("msgid \""), dummy) ||
                 ReadParam(line, _T("msgid\t\""), dummy))
        {
            mstr = dummy.RemoveLast();
            mlinenum = m_textFile->GetCurrentLine() + 1;
            while (!(line = ReadTextLine(m_textFile, m_conv)).IsEmpty())
            {
                if (line[0u] == _T('\t'))
                    line.Remove(0, 1);
                if (line[0u] == _T('"') && line.Last() == _T('"'))
                    mstr += line.Mid(1, line.Length() - 2);
                else
                    break;
            }
        }

        // msgstr:
        else if (ReadParam(line, _T("msgstr \""), dummy) ||
                 ReadParam(line, _T("msgstr\t\""), dummy))
        {
            mtrans = dummy.RemoveLast();
            while (!(line = ReadTextLine(m_textFile, m_conv)).IsEmpty())
            {
                if (line[0u] == _T('\t'))
                    line.Remove(0, 1);
                if (line[0u] == _T('"') && line.Last() == _T('"'))
                    mtrans += line.Mid(1, line.Length() - 2);
                else
                    break;
            }

            if (!OnEntry(mstr, mtrans, mflags, mrefs, mcomment, mautocomments, mlinenum))
                return;

            mcomment = mstr = mtrans = mflags = wxEmptyString;
            mrefs.Clear(); mautocomments.Clear();
        }
        
        // comment:
        else if (line[0u] == _T('#'))
        {
            while (!line.IsEmpty() && line[0u] == _T('#') &&
                   (line.Length() < 2 || (line[1u] != _T(',') && line[1u] != _T(':') && line[1u] != _T('.') )))
            {
                mcomment << line << _T('\n');
                line = ReadTextLine(m_textFile, m_conv);
            }
        }
        
        else
            line = ReadTextLine(m_textFile, m_conv);
    }
}



class CharsetInfoFinder : public CatalogParser
{
    public:
        CharsetInfoFinder(wxTextFile *f, wxMBConv *conv) 
                : CatalogParser(f, conv), m_charset(_T("iso-8859-1")) {}
        wxString GetCharset() const { return m_charset; }

    protected:
        wxString m_charset;
    
        virtual bool OnEntry(const wxString& msgid,
                             const wxString& msgstr,
                             const wxString& flags,
                             const wxArrayString& references,
                             const wxString& comment,
                             const wxArrayString& autocomments,
                             unsigned lineNumber);
};

bool CharsetInfoFinder::OnEntry(const wxString& msgid,
                                const wxString& msgstr,
                                const wxString& flags,
                                const wxArrayString& references,
                                const wxString& comment,
                                const wxArrayString& autocomments, 
                                unsigned lineNumber)
{
    if (msgid.IsEmpty())
    {
        // gettext header:
        Catalog::HeaderData hdr;
        hdr.FromString(msgstr);
        m_charset = hdr.Charset;
        if (m_charset == _T("CHARSET"))
            m_charset = _T("iso-8859-1");
        return false; // stop parsing
    }
    return true;
}



class LoadParser : public CatalogParser
{
    public:
        LoadParser(Catalog *c, wxTextFile *f, wxMBConv *conv)
              : CatalogParser(f, conv), m_catalog(c) {}

    protected:
        Catalog *m_catalog;
    
        virtual bool OnEntry(const wxString& msgid,
                             const wxString& msgstr,
                             const wxString& flags,
                             const wxArrayString& references,
                             const wxString& comment,
                             const wxArrayString& autocomments,
                             unsigned lineNumber);
};


bool LoadParser::OnEntry(const wxString& msgid,
                         const wxString& msgstr,
                         const wxString& flags,
                         const wxArrayString& references,
                         const wxString& comment,
                         const wxArrayString& autocomments,
                         unsigned lineNumber)
{
    if (msgid.IsEmpty())
    {
        // gettext header:
        m_catalog->m_header.FromString(msgstr);
        m_catalog->m_header.Comment = comment;
    }
    else
    {
        CatalogData *d = new CatalogData(wxEmptyString, wxEmptyString);
        if (!flags.IsEmpty()) d->SetFlags(flags);
        d->SetString(msgid);
        d->SetTranslation(msgstr);
        d->SetComment(comment);
        d->SetLineNumber(lineNumber);
        for (size_t i = 0; i < references.GetCount(); i++)
            d->AddReference(references[i]);
        for (size_t i = 0; i < autocomments.GetCount(); i++)
            d->AddAutoComments(autocomments[i]);
        m_catalog->m_data->Put(msgid, d);
        m_catalog->m_dataArray.Add(d);
        m_catalog->m_count++;
    }
    return true;
}



// ----------------------------------------------------------------------
// Catalog class
// ----------------------------------------------------------------------

Catalog::Catalog()
{
    m_data = new wxHashTable(wxKEY_STRING);
    m_count = 0;
    m_isOk = true;
    m_header.BasePath = wxEmptyString;
}


Catalog::~Catalog() 
{
    Clear();
    delete m_data;
}


Catalog::Catalog(const wxString& po_file)
{
    m_data = NULL;
    m_count = 0;
    m_isOk = false;
    Load(po_file);
}


static wxString GetCurrentTimeRFC822()
{
    wxDateTime timenow = wxDateTime::Now();
    int offs = wxDateTime::TimeZone(wxDateTime::Local).GetOffset();
    wxString s;
    s.Printf(_T("%s%s%02i%02i"),
             timenow.Format(_T("%Y-%m-%d %H:%M")).c_str(),
             (offs > 0) ? _T("+") : _T("-"),
             abs(offs) / 3600, (abs(offs) / 60) % 60);
    return s;
}

void Catalog::CreateNewHeader()
{
    HeaderData &dt = Header();
    
    dt.CreationDate = GetCurrentTimeRFC822();
    dt.RevisionDate = dt.CreationDate;

    dt.Language = wxEmptyString;
    dt.Country = wxEmptyString;
    dt.Project = wxEmptyString;
    dt.Team = wxEmptyString;
    dt.TeamEmail = wxEmptyString;
    dt.Charset = _T("utf-8");
    dt.Translator = wxConfig::Get()->Read(_T("translator_name"), wxEmptyString);
    dt.TranslatorEmail = wxConfig::Get()->Read(_T("translator_email"), wxEmptyString);
    dt.SourceCodeCharset = wxEmptyString;

    // NB: keep in sync with Catalog::Update!
    dt.Keywords.Add(_T("_"));
    dt.Keywords.Add(_T("gettext"));
    dt.Keywords.Add(_T("gettext_noop"));

    dt.BasePath = _T(".");

    dt.UpdateDict();
}


bool Catalog::Load(const wxString& po_file)
{
    wxTextFile f;

    Clear();
    delete m_data; m_data = NULL;    
    m_count = 0;
    m_isOk = false;
    m_fileName = po_file;
    m_header.BasePath = wxEmptyString;
    
    /* Load the .po file: */
        
    m_data = new wxHashTable(wxKEY_STRING);

    if (!f.Open(po_file)) return false;
    
    CharsetInfoFinder charsetFinder(&f, &wxConvISO8859_1);
    charsetFinder.Parse();
    m_header.Charset = charsetFinder.GetCharset();

    f.Close();
    wxCSConv encConv(m_header.Charset);
    if (!f.Open(po_file, encConv)) return false;

    LoadParser parser(this, &f, &encConv);
    parser.Parse();

    m_isOk = true;
    
    f.Close();

    /* Load extended information from .po.poedit file, if present: */
    
    if (wxFileExists(po_file + _T(".poedit")) && f.Open(po_file + _T(".poedit")))
    {
        wxString dummy;
        // poedit header (optional, we should be able to read any catalog):
        f.GetFirstLine();
        if (ReadParam(ReadTextLine(&f, NULL),
                      _T("#. Number of items: "), dummy))
        {
            // not used anymore
        }
        ReadParam(ReadTextLine(&f, &encConv),
                  _T("#. Language: "), m_header.Language);
        dummy = ReadTextLine(&f, &encConv);
        if (ReadParam(dummy, _T("#. Country: "), m_header.Country))
            dummy = ReadTextLine(&f, NULL);
        if (ReadParam(dummy, _T("#. Basepath: "), m_header.BasePath))
            dummy = ReadTextLine(&f, NULL);
        ReadParam(dummy, _T("#. SourceCodeCharSet: "), m_header.SourceCodeCharset);

        if (ReadParam(ReadTextLine(&f, NULL), _T("#. Paths: "), dummy))
        {
            long sz;
            dummy.ToLong(&sz);
            for (; sz > 0; sz--)
            if (ReadParam(ReadTextLine(&f, NULL), _T("#.     "), dummy))
                m_header.SearchPaths.Add(dummy);
        }

        if (ReadParam(ReadTextLine(&f, NULL), _T("#. Keywords: "), dummy))
        {
            long sz;
            dummy.ToLong(&sz);
            for (; sz > 0; sz--)
            if (ReadParam(ReadTextLine(&f, NULL), _T("#.     "), dummy))
                m_header.Keywords.Add(dummy);
        }

        f.Close();
    }
    
    return true;
}


void Catalog::Clear()
{
    delete m_data; 
    m_dataArray.Empty();
    m_isOk = true;
    m_count = 0;
    m_data = new wxHashTable(wxKEY_STRING);
}


static bool CanEncodeStringToCharset(const wxString& s, wxMBConv& conv)
{
    if (s.empty())
        return true;
#if !wxUSE_UNICODE
    wxString trans(s.wc_str(wxConvUTF8), conv);
    if (!trans)
        return false;
    #ifdef __WINDOWS__
    // NB: there's an inconsistency in wxWindows (fixed in wx-2.5.2):
    //     wxMBConv conversion rountines return NULL (i.e. empty wxString) in
    //     case of failure on Unix (iconv) but return lossy/inexact conversion
    //     if exact conversion cannot be done on Windows. So we have to do
    //     roundtrip conversion and compare the result with original to see if
    //     the conversion was successful or not:
    wxString roundtrip(trans.wc_str(conv), wxConvUTF8);
    if (roundtrip != s)
        return false;
    #endif
#else
    if (!s.mb_str(conv))
        return false;
    #ifdef __WINDOWS__
    wxString roundtrip(s.mb_str(conv), conv);
    if (roundtrip != s)
        return false;
    #endif
#endif
    return true;
}

static bool CanEncodeToCharset(Catalog& catalog, const wxString& charset)
{
    if (charset == _T("utf-8") || charset == _T("UTF-8"))
        return true;
    
    wxCSConv conv(charset);

    catalog.Header().UpdateDict();
    const Catalog::HeaderData::Entries& hdr(catalog.Header().GetAllHeaders());
    
    for (Catalog::HeaderData::Entries::const_iterator i = hdr.begin();
            i != hdr.end(); ++i)
    {
        if (!CanEncodeStringToCharset(i->Value, conv))
            return false;
    }
    
    size_t cnt = catalog.GetCount();
    for (size_t i = 0; i < cnt; i++)
    {
        if (!CanEncodeStringToCharset(catalog[i].GetTranslation(), conv) ||
            !CanEncodeStringToCharset(catalog[i].GetString(), conv))
        {
            return false;
        }
    }
    return true;
}


static void GetCRLFBehaviour(wxTextFileType& type, bool& preserve)
{
    wxString format = wxConfigBase::Get()->Read(_T("crlf_format"), _T("unix"));

    if (format == _T("win")) type = wxTextFileType_Dos;
    else if (format == _T("mac")) type = wxTextFileType_Mac;
    else if (format == _T("native")) type = wxTextFile::typeDefault;
    else /* _T("unix") */ type = wxTextFileType_Unix;

    preserve = (bool)(wxConfigBase::Get()->Read(_T("keep_crlf"), true));
}

static void SaveMultiLines(wxTextFile &f, const wxString& text)
{
    wxStringTokenizer tkn(text, _T('\n'));
    while (tkn.HasMoreTokens())
        f.AddLine(tkn.GetNextToken());
}

/** Adds \n characters as neccesary for good-looking output
 */
static wxString FormatStringForFile(const wxString& text)
{
    wxString s;
    unsigned n_cnt = 0;
    int len = text.length();
    
    s.Alloc(len + 16);
    // Scan the string up to len-2 because we don't want to account for the
    // very last \n on the line:
    //       "some\n string \n"
    //                      ^
    //                      |
    //                      \--- = len-2
    int i;
    for (i = 0; i < len-2; i++)
    {
        if (text[i] == _T('\\') && text[i+1] == _T('n'))
        {
            n_cnt++;
            s << _T("\\n\"\n\"");
            i++;
        }
        else
            s << text[i];
    }
    // ...and add not yet processed characters to the string...
    for (; i < len; i++)
        s << text[i];
    
    if (n_cnt >= 1)
        return _T("\"\n\"") + s;
    else
        return s;
}

static inline wxString convertUtf8ToCharset(const wxString& s, wxMBConv *conv)
{
#if !wxUSE_UNICODE
    if (conv)
        return wxString(s.wc_str(wxConvUTF8), *conv);
    else
#endif
    return s;
}

bool Catalog::Save(const wxString& po_file, bool save_mo)
{
    CatalogData *data;
    wxString dummy;
    size_t i;
    int j;
    wxTextFileType crlfOrig, crlf;
    bool crlfPreserve;
    wxTextFile f;
    
    GetCRLFBehaviour(crlfOrig, crlfPreserve);

    /* Update information about last modification time: */
 
    m_header.RevisionDate = GetCurrentTimeRFC822();

    /* Detect CRLF format: */

    if (crlfPreserve && wxFileExists(po_file) && f.Open(po_file))
    {
        {
        wxLogNull null;
        crlf = f.GuessType();
        }
        if (crlf == wxTextFileType_None || crlf == wxTextFile::typeDefault)
            crlf = crlfOrig;
        f.Close();
    }
    else
        crlf = crlfOrig;

    /* Save .po file: */

    wxString charset(m_header.Charset);
    if (!charset)
        charset = _T("utf-8");
        
    if (!CanEncodeToCharset(*this, charset))
    {
        wxString msg;
        msg.Printf(_("The catalog couldn't be saved in '%s' charset as\nspecified in catalog settings. It was saved in UTF-8 instead\nand the setting was modified accordingly."), charset.c_str());
        wxMessageBox(msg, _("Error saving catalog"),
                     wxOK | wxICON_EXCLAMATION);
        charset = _T("utf-8");
    }
    m_header.Charset = charset;
   
    if (!wxFileExists(po_file) || !f.Open(po_file))
        if (!f.Create(po_file))
            return false;
    for (j = f.GetLineCount() - 1; j >= 0; j--)
        f.RemoveLine(j);
   

    wxCSConv *encConv;
#if wxUSE_UNICODE
    encConv = new wxCSConv(charset);
#else
    if (wxStricmp(charset, _T("utf-8")) != 0)
        encConv = new wxCSConv(charset);
    else
        encConv = NULL;
#endif

    SaveMultiLines(f, convertUtf8ToCharset(m_header.Comment, encConv));
    f.AddLine(_T("msgid \"\""));
    f.AddLine(_T("msgstr \"\""));
    wxString pohdr = wxString(_T("\"")) +
               convertUtf8ToCharset(m_header.ToString(_T("\"\n\"")), encConv);
    pohdr.RemoveLast();
    SaveMultiLines(f, pohdr);
    f.AddLine(wxEmptyString);

    for (i = 0; i < m_dataArray.GetCount(); i++)
    {
        data = &(m_dataArray[i]);
        SaveMultiLines(f, convertUtf8ToCharset(data->GetComment(), encConv));
        for (unsigned i = 0; i < data->GetAutoComments().GetCount(); i++)
            f.AddLine(_T("#. ") + data->GetAutoComments()[i]);
        for (unsigned i = 0; i < data->GetReferences().GetCount(); i++)
            f.AddLine(_T("#: ") + data->GetReferences()[i]);
        dummy = data->GetFlags();
        if (!dummy.IsEmpty())
            f.AddLine(dummy);
        dummy = convertUtf8ToCharset(FormatStringForFile(data->GetString()),
                                     encConv);
        data->SetLineNumber(f.GetLineCount()+1);
        SaveMultiLines(f, _T("msgid \"") + dummy + _T("\""));
        dummy = convertUtf8ToCharset(
                    FormatStringForFile(data->GetTranslation()),
                    encConv);
        SaveMultiLines(f, _T("msgstr \"") + dummy + _T("\""));
        f.AddLine(wxEmptyString);
    }
    
#if wxUSE_UNICODE
    f.Write(crlf, *encConv);
#else
    f.Write(crlf);
#endif
    f.Close();

    /* If neccessary, save extended info into .po.poedit file: */

    if (!m_header.Language.IsEmpty() || 
        !m_header.Country.IsEmpty() || 
        !m_header.BasePath.IsEmpty() || 
        m_header.SearchPaths.GetCount() > 0 ||
        m_header.Keywords.GetCount() > 0)
    {
        if (!wxFileExists(po_file + _T(".poedit")) || !f.Open(po_file + _T(".poedit")))
            if (!f.Create(po_file + _T(".poedit")))
                return false;
        for (j = f.GetLineCount() - 1; j >= 0; j--)
            f.RemoveLine(j);

        f.AddLine(_T("#. This catalog was generated by poedit"));
        dummy.Printf(_T("#. Number of items: %i"), GetCount());
        f.AddLine(dummy);
        f.AddLine(_T("#. Language: ") + 
                  convertUtf8ToCharset(m_header.Language, encConv));
        f.AddLine(_T("#. Country: ") + 
                  convertUtf8ToCharset(m_header.Country, encConv));
        f.AddLine(_T("#. Basepath: ") + m_header.BasePath);
        f.AddLine(_T("#. SourceCodeCharSet: ") + m_header.SourceCodeCharset);

        dummy.Printf(_T("#. Paths: %i"), m_header.SearchPaths.GetCount());
        f.AddLine(dummy);
        for (i = 0; i < m_header.SearchPaths.GetCount(); i++)
            f.AddLine(_T("#.     ") + m_header.SearchPaths[i]);

        dummy.Printf(_T("#. Keywords: %i"), m_header.Keywords.GetCount());
        f.AddLine(dummy);
        for (i = 0; i < m_header.Keywords.GetCount(); i++)
            f.AddLine(_T("#.     ") + m_header.Keywords[i]);
        f.Write(crlf);
        f.Close();
    }
    
    
    delete encConv;    

    /* If user wants it, compile .mo file right now: */
    
    if (save_mo && wxConfig::Get()->Read(_T("compile_mo"), true))
        ExecuteGettext(_T("msgfmt -c -o \"") + 
                       po_file.BeforeLast(_T('.')) + _T(".mo\" \"") + po_file + _T("\""));

    m_fileName = po_file;
    
    return true;
}


bool Catalog::Update()
{
    if (!m_isOk) return false;

    ProgressInfo pinfo;
    pinfo.SetTitle(_("Updating catalog..."));

    wxString cwd = wxGetCwd();
    if (m_fileName != wxEmptyString) 
    {
        wxString path;
        
        if (wxIsAbsolutePath(m_header.BasePath))
            path = m_header.BasePath;
        else
            path = wxPathOnly(m_fileName) + _T("/") + m_header.BasePath;
        
        if (wxIsAbsolutePath(path))
            wxSetWorkingDirectory(path);
        else
            wxSetWorkingDirectory(cwd + _T("/") + path);
    }
    
    SourceDigger dig(&pinfo);
    
    wxArrayString keywords;
    if (m_header.Keywords.IsEmpty())
    {
        // NB: keep in sync with Catalog::CreateNewHeader!
        keywords.Add(_T("_"));
        keywords.Add(_T("gettext"));
        keywords.Add(_T("gettext_noop"));
    }
    else
    {
        WX_APPEND_ARRAY(keywords, m_header.Keywords);
    }
    
    Catalog *newcat = dig.Dig(m_header.SearchPaths,
                              keywords,
                              m_header.SourceCodeCharset);

    if (newcat != NULL) 
    {
        bool succ = false;
        pinfo.UpdateMessage(_("Merging differences..."));

        if (ShowMergeSummary(newcat))
            succ = Merge(newcat);
        if (!succ)
        {
            delete newcat;
            newcat = NULL;
        }
    }
    
    wxSetWorkingDirectory(cwd);
    
    if (newcat == NULL) return false;
  
    delete newcat;
    return true;
}


bool Catalog::UpdateFromPOT(const wxString& pot_file)
{
    if (!m_isOk) return false;

    Catalog newcat(pot_file);

    if (!newcat.IsOk())
    {
        wxLogError(_("'%s' is not a valid POT file."), pot_file.c_str());
        return false;
    }

    if (ShowMergeSummary(&newcat))
        return Merge(&newcat);
    else
        return false;
}


bool Catalog::Merge(Catalog *refcat)
{
    wxString oldname = m_fileName;
    wxString tmp1 = wxGetTempFileName(_T("poedit"));
    wxString tmp2 = wxGetTempFileName(_T("poedit"));
    wxString tmp3 = wxGetTempFileName(_T("poedit"));

    refcat->Save(tmp1, false);
    Save(tmp2, false);
    
    bool succ =
        ExecuteGettext(_T("msgmerge --force-po -o \"") + tmp3 + _T("\" \"") + 
                       tmp2 + _T("\" \"") + tmp1 + _T("\""));
    if (succ)
    {
        Catalog *c = new Catalog(tmp3);
        Clear();
        Append(*c);
        delete c;
    }

    wxRemoveFile(tmp1);
    wxRemoveFile(tmp2);
    wxRemoveFile(tmp3);
    wxRemoveFile(tmp1 + _T(".poedit"));
    wxRemoveFile(tmp2 + _T(".poedit"));
    
    m_fileName = oldname;

    return succ;
}


void Catalog::GetMergeSummary(Catalog *refcat, 
                          wxArrayString& snew, wxArrayString& sobsolete)
{
    snew.Empty();
    sobsolete.Empty();
    unsigned i;
    
    for (i = 0; i < GetCount(); i++)
        if (refcat->FindItem((*this)[i].GetString()) == NULL)
        sobsolete.Add((*this)[i].GetString());

    for (i = 0; i < refcat->GetCount(); i++)
        if (FindItem((*refcat)[i].GetString()) == NULL)
        snew.Add((*refcat)[i].GetString());
}
        
bool Catalog::ShowMergeSummary(Catalog *refcat)
{    
    if (wxConfig::Get()->Read(_T("show_summary"), true))
    {
        wxArrayString snew, sobsolete;
        GetMergeSummary(refcat, snew, sobsolete);
        MergeSummaryDialog sdlg;
        sdlg.TransferTo(snew, sobsolete);
        return (sdlg.ShowModal() == wxID_OK);
    }
    else
        return true;
}


CatalogData* Catalog::FindItem(const wxString& key) const
{
    return (CatalogData*) m_data->Get(key);
}


bool Catalog::Translate(const wxString& key, const wxString& translation)
{
    CatalogData *d = FindItem(key);
    
    if (d == NULL) 
        return false;
    else
    {
        d->SetTranslation(translation);
        return true;
    }
}


void Catalog::Append(Catalog& cat)
{
    CatalogData *dt, *mydt;
    
    for (unsigned i = 0; i < cat.GetCount(); i++)
    {
        dt = &(cat[i]);
        mydt = FindItem(dt->GetString());
        if (mydt == NULL)
        {
            mydt = new CatalogData(*dt);
            m_data->Put(dt->GetString(), mydt);
            m_dataArray.Add(mydt);
            m_count++;
        }
        else
        {
            for (unsigned j = 0; j < dt->GetReferences().GetCount(); j++)
                mydt->AddReference(dt->GetReferences()[j]);
            if (!dt->GetTranslation().IsEmpty())
                mydt->SetTranslation(dt->GetTranslation());
            if (dt->IsFuzzy()) mydt->SetFuzzy(true);
        }
    }
}


void Catalog::GetStatistics(int *all, int *fuzzy, int *badtokens, int *untranslated)
{
    if (all) *all = 0;
    if (fuzzy) *fuzzy = 0;
    if (badtokens) *badtokens = 0;
    if (untranslated) *untranslated = 0;
    for (size_t i = 0; i < GetCount(); i++)
    {
        if (all) (*all)++;
        if ((*this)[i].IsFuzzy()) (*fuzzy)++;
        if ((*this)[i].HasBadTokens()) (*badtokens)++;
        if (!(*this)[i].IsTranslated()) (*untranslated)++;
    }
}


void CatalogData::SetFlags(const wxString& flags)
{
    m_isFuzzy = false;
    m_moreFlags.Empty();
    
    if (flags.IsEmpty()) return;
    wxStringTokenizer tkn(flags.Mid(1), _T(" ,"), wxTOKEN_STRTOK);
    wxString s;
    while (tkn.HasMoreTokens())
    {
        s = tkn.GetNextToken();
        if (s == _T("fuzzy")) m_isFuzzy = true;
        else m_moreFlags << _T(", ") << s;
    }
}


wxString CatalogData::GetFlags() const
{
    wxString f;
    if (m_isFuzzy) f << _T(", fuzzy");
    f << m_moreFlags;
    if (!f.IsEmpty()) 
        return _T("#") + f;
    else 
        return wxEmptyString;
}

// This regex is used by CatalogData to extract the tokens
// since there is no need to have one object per instance of CatalogData, 
// we declare it static
wxRegEx CatalogData::ms_tokenExtraction(
        _T(".*(%[-\\+0 #]?[0-9]?[\\.[0-9]*]?([cCdiouxXeEfgGpsS]|ld)).*"),
        wxRE_EXTENDED | wxRE_NEWLINE);

bool CatalogData::CheckPrintfCorrectness()
{
    // Added by Fr�d�ric Giudicelli (info@newpki.org)
    // To verify the validity of tokens.

#if wxUSE_UNICODE && !wxCHECK_VERSION(2,5,1)
    #warning "validity checking disabled due to broken wxRegEx"
#else    
    // - We use m_tokenExtraction the extract the tokens 
    //   from a string. 
    // - With the extracted token we generate a regexp.    
    // - Using the generated regexp we validate the string.
    
    wxASSERT_MSG( ms_tokenExtraction.IsValid(),
                  _T("error in validity checking regex") );

    // We make sure that the translated string has all the tokens
    // the original string declared
    if (!ValidateTokensString(m_string, m_translation))
        return false;

    // We make sure that the translated string doesn't have more
    // tokens than the original string
    if (!ValidateTokensString(m_translation, m_string))
        return false;
#endif

    return true;
}

bool CatalogData::ValidateTokensString(const wxString& from,
                                       const wxString& to)
{
    wxString subString;
    wxString newPattern;
    size_t startOff;
    size_t tokenLen;
    wxString currMatch;
    wxRegEx translationValidation;
    int tokenCount;

    subString = from;
    // Remove the double %, we don't need them
    subString.Replace(_T("%%"), wxEmptyString);

    tokenCount = 0;
    
    // We first extact all the tokens declarations
    // regexp allways returns the last occurence, so we start
    // from the end
    while (subString.Length() && ms_tokenExtraction.Matches(subString))
    {
        if (!ms_tokenExtraction.GetMatch(&startOff, &tokenLen, 1))
            return false;

		// Get the token declaration
        currMatch = subString.SubString(startOff, startOff+tokenLen-1);
        if (currMatch.IsEmpty())
            return false;

		// Remove then newly extracted token from the string
        subString = subString.Left(startOff);
        
        currMatch.Replace(_T("."), _T("\\."));
        currMatch.Replace(_T("+"), _T("\\+"));

        // Append the new token to the regexp pattern
        currMatch += _T(".*");
        currMatch += newPattern;

        newPattern = currMatch;
        tokenCount++;
    }

	// We didn't find any token, no need to validate
    if (!tokenCount)
        return true;

    currMatch = _T(".*");
    currMatch += newPattern;

    // We now validate the destination string using the generated regexp
    // pattern.  We compile the generated regexp pattern.
    if (!translationValidation.Compile(currMatch, wxRE_EXTENDED | wxRE_NOSUB))
    {
        return false;
    }

    subString = to;
    // Remove the double %, we don't need them
    subString.Replace(_T("%%"), wxEmptyString);
    if(!translationValidation.Matches(subString))
        return false;

    return true;
}
