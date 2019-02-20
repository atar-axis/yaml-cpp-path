/*
MIT License

Copyright(c) 2019 Peter Hauptmann

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files(the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions :

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include "yaml-path.h"
#include "yaml-path-internals.h"
#include <yaml-cpp/yaml.h>
#include <assert.h>

namespace YAML
{

   // ----- PathException
   std::string const & PathException::What() const
   {
      if (!m_what.length())
         return m_what;
      switch (m_error)
      {
      case EPathError::None:   return m_what = "OK";

      case EPathError::InvalidToken:
         return m_what = (std::stringstream() << "Invalid Token at position " << m_offset << ": " << m_value).str();

      case EPathError::InvalidIndex:
         return m_what = (std::stringstream() << "Index expected at position " << m_offset << ": " << m_value).str();

      case EPathError::UnexpectedEnd:
         return m_what = (std::stringstream() << "unexpected end of path at position " << m_offset << ": " << m_value).str();

      case EPathError::InvalidNodeType:
         return m_what = (std::stringstream() << "node type mismatch at path position " << m_offset << ": " << m_value).str();

      case EPathError::NodeNotFound:
         return m_what = (std::stringstream() << "node not found at path position " << m_offset << ": " << m_value).str();

      default:
         return m_what = (std::stringstream() << "Undefined exception #" << (int)m_error << " at offset " << m_offset << ": " << m_value).str();
      }
   }

   void PathException::ThrowDerived()
   {
      switch (m_error)
      {
         case EPathError::None: assert(false);  return;
         case EPathError::Internal: throw PathInternalException(m_offset, m_value);
         case EPathError::InvalidToken: throw PathInvalidTokenException(m_offset, m_value);
         case EPathError::InvalidIndex: throw PathInvalidIndexException(m_offset, m_value);
         case EPathError::UnexpectedEnd: throw PathUnexpectedEndException(m_offset, m_value);

         case EPathError::InvalidNodeType: throw PathInvalidNodeTypeException(m_offset, m_value);
         case EPathError::NodeNotFound: throw PathNodeNotFoundException(m_offset, m_value);
         default: throw *this;
      }
   }

   namespace YamlPathDetail
   {
      template <typename T2, typename TEnum>
      T2 MapValue(TEnum value, std::initializer_list<std::pair<TEnum, T2>> values, T2 dflt = T2())
      {
         for (auto && p : values)
            if (p.first == value)
               return p.second;
         return dflt;
      }

      template <typename T2, typename TBit, typename TMask>
      std::string MapBitMask(TMask value, std::initializer_list<std::pair<TBit, T2>> values, T2 sep = ", ")
      {
         std::stringstream result;
         bool noSep = true;
         auto Sep = [&]() -> std::stringstream & { if (!Exchange(noSep, false)) result << sep; return result; };
         for (auto && p : values)
         {
            TMask mask = ((TMask)1) << static_cast<TMask>(p.first);
            if (value & mask)
            {
               Sep() << p.second;
               value &= ~mask;
            }
         }
         if (value != 0)
            Sep() << "(" << std::hex << value << "h)";
         return result.str();
      }

      std::initializer_list<std::pair<EToken, char const *>> MapETokenName =
      {
         { EToken::OpenBracket, "open bracket"},
         { EToken::CloseBracket, "closing bracket" },
         { EToken::Equal, "equal" },
         { EToken::None, "end of path" },
         { EToken::Period, "period" },
         { EToken::QuotedIdentifier, "quoted identifier" },
         { EToken::UnquotedIdentifier, "unquoted identifier" },
      };

      std::initializer_list<std::pair<NodeType::value, char const *>> MapNodeTypeName =
      {
         { NodeType::Map, "map" },
         { NodeType::Sequence, "sequence" },
         { NodeType::Scalar , "scalar" },
         { NodeType::Null, "(null)" },
         { NodeType::Undefined, "(undefined)" },
      };

      std::initializer_list<std::pair<ESelector, char const *>> MapESelectorName =
      {
         { ESelector::Index,  "index" },
         { ESelector::Key,    "key" },
         { ESelector::SeqMapFilter, "seq-map filter" },
         { ESelector::None, "(none)" },
         { ESelector::Invalid, "(invalid)" },
      };

      std::initializer_list<std::pair<EPathError, char const *>> MapEPathErrorName =
      {
         { EPathError::None ,             "(none)" },
         { EPathError::Internal,          "(internal, please report)" },
         { EPathError::InvalidIndex,      "invalid index" },
         { EPathError::InvalidNodeType,   "selector cannot not match node type" },
         { EPathError::InvalidToken,      "invalid token" },
         { EPathError::NodeNotFound,      "no node matches selector" },
         { EPathError::UnexpectedEnd,     "unexpected end of path" },
      };

      // ----- Utility functions

      /// Creates an undefined YAML node (<code>(bool)UndefinedNode() == false</code>)
      Node UndefinedNode()
      {
         static auto undefinedNode = Node()["x"];
         return undefinedNode;
      }

      /// result = target; target = newValue
      template <typename T1, typename T2>
      T1 Exchange(T1 & target, T2 newValue)
      {
         T1 result = std::move(target);
         target = std::move(newValue);
         return std::move(result);
      }


      /** splits path at offset, 
          returning everything left of [offset], assigning everything right of it to \c path. 
      */
      path_arg SplitAt(path_arg & path, size_t offset)
      {
         if (offset == 0)
            return path_arg();

         if (offset >= path.size())
            return Exchange(path, path_arg());

         path_arg result = path.substr(0, offset);
         path = path.substr(offset);
         return result;
      }

      // ----- TokenScanner
      EToken GetSingleCharToken(char c, std::initializer_list<std::pair<char, EToken>> values)
      {
         for (auto && v : values)
            if (v.first == c)
               return v.second;
         return EToken::None;
      }


      TokenData const & PathScanner::SetToken(EToken id, path_arg p)
      {
         // Generate error when ValidTokens are specified:
         m_curToken = { id, std::move(p) };

         /* skipping whitespace after token, so that if this was the last token,
            we get to the end of the string and operator bool becomes false */
         SkipWS();
         return m_curToken;
      }

      void PathScanner::SkipWS()
      {
         // non-ascii chars are NOT considered whitespace
         Split(m_rpath, [](char c) { return isascii(c) && isspace(c); });
      }

      inline PathScanner::PathScanner(path_arg p) : m_rpath(p), m_all(p)
      {
         SkipWS();
      }

      TokenData const & PathScanner::NextToken()
      {
         if (m_rpath.empty())
            return SetToken(EToken::None, path_arg());

         if (m_curException)
            return m_curToken;

         // single-char special tokens
         char head = m_rpath[0];
         EToken t = GetSingleCharToken(head, {
            { '.', EToken::Period },
            { '[', EToken::OpenBracket },
            { ']', EToken::CloseBracket },
            { '=', EToken::Equal },
            });

         if (t != EToken::None)
            return SetToken(t, SplitAt(m_rpath, 1));

         // quoted token
         if (head == '\'' || head == '"')
         {
            size_t end = m_rpath.find(m_rpath[0], 1);
            if (end == std::string::npos)
               return SetToken(EToken::Invalid, path_arg());

            return SetToken(EToken::QuotedIdentifier, SplitAt(m_rpath, end + 1).substr(1, end - 1));
         }

         // unquoted token
         // unquoted token. non-ascii characters ARE treated as part of the token.
         auto result = Split(m_rpath, [](char c) { return !isascii(c) || !(isspace(c) || ispunct(c)); });
         if (result.empty())
            return SetError(EPathError::InvalidToken), m_curToken;

         return SetToken(EToken::UnquotedIdentifier, result);
      }

      EPathError PathScanner::SetError(EPathError error)
      {
         assert(error != EPathError::None);
         m_curException = PathException(error, ScanOffset(), std::string(m_curToken.value));
         m_curToken = { EToken::Invalid };
         SetSelector(ESelector::Invalid, ArgNull{});
         return error;
      }

      std::optional<size_t> PathScanner::AsIndex()
      {
         if (m_curToken.id != EToken::UnquotedIdentifier)
            return std::nullopt;

         size_t value = 0;
         size_t index = 0;
         for(auto c : m_curToken.value)
         {
            ++index;
            if (c < '0' || c >'9')
               return std::nullopt;

            size_t prev = value;
            value = value * 10 + (c - '0');
            if (value < prev)  // unsigned integer overflow
               return SetError(EPathError::InvalidIndex), std::nullopt;
         }
         return value;
      }

      bool PathScanner::NextSelectorToken(uint64_t validTokens, EPathError error)
      {
         if (!m_tokenPending)
            NextToken();
         m_tokenPending = false;

         if (BitsContain(validTokens, m_curToken.id))
            return true;

         SetError(error);
         return false;
      }

      ESelector PathScanner::NextSelector()
      {
         // sticky on error
         if (m_curException)
            return ESelector::Invalid;


         // skip period if allowed at this point
         if (m_periodAllowed)
         {
            if (!NextSelectorToken(ValidTokensAtStart | BitsOf({ EToken::Period })))
               return ESelector::Invalid;
            m_periodAllowed = false;
            
            if (m_curToken.id == EToken::Period)
               m_selectorRequired = true;    // path cannot end with period after selector
            else
               m_tokenPending = true;        // if not a token, stuff this token back
         }

         // Next token
         if (!NextSelectorToken(ValidTokensAtStart))
            return ESelector::Invalid;

         // analyze token
         switch (m_curToken.id)
         {
            case EToken::None:
               if (m_selectorRequired)
                  return SetError(EPathError::UnexpectedEnd), ESelector::Invalid;
               return ESelector::None;

            case EToken::QuotedIdentifier:
            case EToken::UnquotedIdentifier:
            {
               SetSelector(ESelector::Key, ArgKey{ m_curToken.value });
               m_leftOffset = ScanOffset();
               m_periodAllowed = true;
               return m_selector;
            }

            case EToken::OpenBracket:
            {
               if (!NextSelectorToken(BitsOf({ EToken::UnquotedIdentifier, EToken::QuotedIdentifier }), EPathError::InvalidIndex))
                  return ESelector::Invalid;

               // [1] --> index
               auto index = AsIndex();
               if (index)
               {
                  if (!NextSelectorToken(BitsOf({ EToken::CloseBracket })))
                     return ESelector::Invalid;

                  m_periodAllowed = true;
                  m_leftOffset = ScanOffset();
                  return SetSelector(ESelector::Index, ArgIndex{ *index });
               }
               if (m_curException)
                  return ESelector::Invalid;

               // [key=] or [key=value] for filtering
               auto tokKey = m_curToken.value;

               if (!NextSelectorToken(BitsOf({ EToken::Equal })))
                  return ESelector::Invalid;

               std::optional<path_arg> tokValue = std::nullopt;
               if (!NextSelectorToken(BitsOf({ EToken::QuotedIdentifier, EToken::UnquotedIdentifier, EToken::CloseBracket })))
                  return ESelector::Invalid;
               if (m_curToken.id != EToken::CloseBracket)
               {
                  tokValue = m_curToken.value;
                  if (!NextSelectorToken(BitsOf({ EToken::CloseBracket })))
                     return ESelector::Invalid;
               }

               m_periodAllowed = true;
               m_leftOffset = ScanOffset();
               return SetSelector(ESelector::SeqMapFilter, ArgSeqMapFilter{tokKey, tokValue});
            }
         }
         return ESelector::Invalid;
      }
   } // YamlPathDetail


   /** validates the syntax of a YAML path. returns an error for invalid path, or EPathError::None, if the path is valid
   */
   EPathError PathValidate(path_arg p, path_arg * valid, size_t * scanOffs)
   {
      YamlPathDetail::PathScanner scan(p);
      while (scan)
         scan.NextSelector();

      if (valid)
         *valid= scan.Left();

      if (scanOffs)
         *scanOffs = scan.ScanOffset();
      return scan.CurrentException() ? scan.CurrentException()->Error() : EPathError::None;
   }

   namespace
   {
      bool IsMatch(Node node)
      {
         return !(!node ||
            node.IsNull() ||
            (node.IsSequence() && node.size() == 0) ||
            (node.IsMap() && node.size() == 0));
      }

      EPathError SeqMapByKey(Node & node, path_arg key)
      {
         /**
         \todo Optimization: YAML::Node could uses a reserve
         \todo Optimization: convert<string_view> without copy to string
         */
         YAML::Node newNode;
         if (node.IsSequence())
         {
            for (auto && el : node)
               if (el.IsMap())
               {
                  Node value = el[std::string(key)];
                  if (value)
                     newNode.push_back(value);
               }
         }
         else if (node.IsMap())
            newNode = node[std::string(key)];
         else
            return EPathError::InvalidNodeType;

         if (!IsMatch(newNode))
            return EPathError::NodeNotFound;

         node.reset(newNode);
         return EPathError::None;
      }

      bool prelim_node_eq(Node const & n, path_arg p)
      {
         /// \todo default to case insensitive comparison
         /// \todo parametrize to case sensitive, partial match
         return n.IsScalar() && n.as<std::string>() == p;
      }

      bool SeqMapFilterMatchElement(Node const & element, YamlPathDetail::ArgSeqMapFilter const & arg)
      {
         if (!element.IsMap())
            return false;

         auto v = element[std::string(arg.key)];
         if (!v || !v.IsScalar())
            return false;

         return !arg.value ||    // any value accepted as long as key exists
                prelim_node_eq(v, *arg.value);
      }

      EPathError SeqMapFilter(Node & node, YamlPathDetail::ArgSeqMapFilter const & arg)
      {
         Node newNode;
         if (node.IsSequence())
         {
            for (auto && el : node)
               if (SeqMapFilterMatchElement(el, arg))
                  newNode.push_back(el);
         }
         else if (node.IsMap())
         {
            if (SeqMapFilterMatchElement(node, arg))
               newNode = node;
         }
         else
            return EPathError::InvalidNodeType;

         if (!IsMatch(newNode))
            return EPathError::NodeNotFound;

         node.reset(newNode);
         return EPathError::None;
      }
   }

   EPathError PathResolve(Node & node, path_arg & path)
   {
      using namespace YamlPathDetail;
      PathScanner scan(path);

      while (scan)
      {
         if (!node)
            return EPathError::NodeNotFound;

         path = scan.Right(); // path is updated only when both the selector is valid, and it selects a valid node. 
            
         switch (scan.NextSelector())
         {
            case ESelector::Key:
            if (auto err = SeqMapByKey(node, scan.SelectorData<ArgKey>().key); err != EPathError::None)
               return err;
            continue;

            case ESelector::Index:
            {
               size_t index = scan.SelectorData<ArgIndex>().index;
               if (node.IsScalar())
               {
                  if (index != 0)   // for scalar node, [0] sticks to the node
                     return EPathError::NodeNotFound;
                  continue;
               }
               if (node.IsSequence())
               {
                  if (index >= node.size())
                     return EPathError::NodeNotFound;

                  node.reset(node[index]);
                  continue;
               }
               return EPathError::InvalidNodeType;     // cannot go in
            }

            case ESelector::SeqMapFilter:
            {
               if (auto err = SeqMapFilter(node, scan.SelectorData<ArgSeqMapFilter>()); err != EPathError::None)
                  return err;
               continue;
            }

            default:
               assert(false);    // no other selectors supported right now
         }
      }
      path = scan.Right();
      return EPathError::None;
   }

   Node Select(YAML::Node node, path_arg path)
   {
      PathResolve(node, path);
      return path.length() ? YamlPathDetail::UndefinedNode() : node;
   }
} // namespace YAML


/**

\todo PathValidate, PathResolve etc. swallow some diagnostics. 
      "CurrentException" needs to be replaced / refactored. 
      PathResolve etc. should (optionally) provide diagnostics (both for parse errors, and node errors)

\todo names:
   - PathError is now a path-or-node error
   - token scanner now scans both tokens and selectors
   - Should "PathAt" be called" Select"?
*/
