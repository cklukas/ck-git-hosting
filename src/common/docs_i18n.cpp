// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#include "ckgit/docs_i18n.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

namespace ckgit {
namespace {

using Field = std::string DocsUiText::*;

// Every key, in declaration order, with the member it sets.
const std::vector<std::pair<std::string_view, Field>>& fields() {
  static const std::vector<std::pair<std::string_view, Field>> kFields{
      {"skip_to_content", &DocsUiText::skip_to_content},
      {"menu", &DocsUiText::menu},
      {"home", &DocsUiText::home},
      {"on_this_page", &DocsUiText::on_this_page},
      {"previous", &DocsUiText::previous},
      {"next", &DocsUiText::next},
      {"previous_next", &DocsUiText::previous_next},
      {"search", &DocsUiText::search},
      {"search_label", &DocsUiText::search_label},
      {"site_index", &DocsUiText::site_index},
      {"other_pages", &DocsUiText::other_pages},
      {"built_with", &DocsUiText::built_with},
      {"sections", &DocsUiText::sections},
      {"pages", &DocsUiText::pages},
      {"breadcrumb", &DocsUiText::breadcrumb},
      {"links", &DocsUiText::links},
      {"sites", &DocsUiText::sites},
      {"documentation_sites", &DocsUiText::documentation_sites},
      {"languages", &DocsUiText::languages},
      {"not_translated", &DocsUiText::not_translated},
      {"tags", &DocsUiText::tags},
      {"tagged", &DocsUiText::tagged},
      {"page_count", &DocsUiText::page_count},
      {"page_count_one", &DocsUiText::page_count_one},
      {"copy_code", &DocsUiText::copy_code},
      {"copied", &DocsUiText::copied},
      {"moved", &DocsUiText::moved},
      {"note", &DocsUiText::note},
      {"tip", &DocsUiText::tip},
      {"important", &DocsUiText::important},
      {"warning", &DocsUiText::warning},
      {"caution", &DocsUiText::caution},
      {"info", &DocsUiText::info},
      {"danger", &DocsUiText::danger},
      {"details", &DocsUiText::details},
      {"footnotes", &DocsUiText::footnotes},
      {"back_to_reference", &DocsUiText::back_to_reference},
      {"overview", &DocsUiText::overview},
      {"features", &DocsUiText::features},
      {"search_open", &DocsUiText::search_open},
      {"search_close", &DocsUiText::search_close},
      {"theme", &DocsUiText::theme},
      {"theme_system", &DocsUiText::theme_system},
      {"theme_light", &DocsUiText::theme_light},
      {"theme_dark", &DocsUiText::theme_dark},
      {"diagram_open", &DocsUiText::diagram_open},
      {"diagram_expand", &DocsUiText::diagram_expand},
      {"diagram_close", &DocsUiText::diagram_close},
      {"markdown_version", &DocsUiText::markdown_version},
      {"llms_index", &DocsUiText::llms_index},
  };
  return kFields;
}

// (The texts are a vector, not an initializer_list: a list's backing array
// would not outlive the static initializer that builds the table.)
struct Translation {
  std::string_view lang;
  std::vector<std::pair<std::string_view, std::string_view>> texts;
};

// The built-in sets. Each lists every key; English is DocsUiText's defaults.
// Languages whose plural forms depend on the number say "pages: N" rather
// than inflecting a noun after it.
const std::vector<Translation>& translations() {
  static const std::vector<Translation> kTranslations{
      {"de",
       {{"skip_to_content", "Zum Inhalt springen"}, {"menu", "Menü"}, {"home", "Startseite"}, {"on_this_page", "Auf dieser Seite"},
        {"previous", "Zurück"}, {"next", "Weiter"}, {"previous_next", "Vorherige und nächste Seite"}, {"search", "Suchen"},
        {"search_label", "Diese Website durchsuchen"}, {"site_index", "Seitenverzeichnis"}, {"other_pages", "Weitere Seiten"},
        {"built_with", "Erstellt mit {generator}"}, {"sections", "Bereiche"}, {"pages", "Seiten"},
        {"breadcrumb", "Navigationspfad"}, {"links", "Links"}, {"sites", "Websites"},
        {"documentation_sites", "Dokumentationswebsites"}, {"languages", "Sprachen"},
        {"not_translated", "Diese Seite wurde noch nicht übersetzt ({language}); sie wird in der Originalsprache angezeigt ({default})."},
        {"tags", "Schlagwörter"}, {"tagged", "Schlagwort „{tag}“"}, {"page_count", "{count} Seiten"},
        {"page_count_one", "1 Seite"}, {"copy_code", "Code kopieren"}, {"copied", "Kopiert"},
        {"moved", "Diese Seite ist umgezogen nach"}, {"note", "Hinweis"}, {"tip", "Tipp"}, {"important", "Wichtig"},
        {"warning", "Warnung"}, {"caution", "Vorsicht"}, {"info", "Info"}, {"danger", "Gefahr"}, {"details", "Details"},
        {"footnotes", "Fußnoten"}, {"back_to_reference", "Zurück zum Verweis"}, {"overview", "Übersicht"}, {"features", "Funktionen"}, {"search_open", "Suche öffnen"}, {"search_close", "Suche schließen"}, {"theme", "Farbschema"}, {"theme_system", "Systemeinstellung"}, {"theme_light", "Hell"}, {"theme_dark", "Dunkel"}, {"diagram_open", "Diagramm öffnen"}, {"diagram_expand", "Diagramm vergrößern"}, {"diagram_close", "Diagramm schließen"}, {"markdown_version", "Diese Seite als reines Markdown lesen"}, {"llms_index", "Das Dokumentationsverzeichnis für Agenten lesen"}}},
      {"fr",
       {{"skip_to_content", "Aller au contenu"}, {"menu", "Menu"}, {"home", "Accueil"}, {"on_this_page", "Sur cette page"},
        {"previous", "Précédent"}, {"next", "Suivant"}, {"previous_next", "Page précédente et suivante"},
        {"search", "Rechercher"}, {"search_label", "Rechercher sur ce site"}, {"site_index", "Index du site"},
        {"other_pages", "Autres pages"}, {"built_with", "Créé avec {generator}"}, {"sections", "Sections"},
        {"pages", "Pages"}, {"breadcrumb", "Fil d’Ariane"}, {"links", "Liens"}, {"sites", "Sites"},
        {"documentation_sites", "Sites de documentation"}, {"languages", "Langues"},
        {"not_translated", "Cette page n’a pas encore été traduite ({language}) ; elle est affichée dans sa langue d’origine ({default})."},
        {"tags", "Étiquettes"}, {"tagged", "Étiquette « {tag} »"}, {"page_count", "{count} pages"},
        {"page_count_one", "1 page"}, {"copy_code", "Copier le code"}, {"copied", "Copié"},
        {"moved", "Cette page a été déplacée vers"}, {"note", "Remarque"}, {"tip", "Astuce"}, {"important", "Important"},
        {"warning", "Avertissement"}, {"caution", "Attention"}, {"info", "Info"}, {"danger", "Danger"},
        {"details", "Détails"}, {"footnotes", "Notes de bas de page"}, {"back_to_reference", "Retour à l’appel de note"}, {"overview", "Vue d’ensemble"}, {"features", "Fonctionnalités"}, {"search_open", "Ouvrir la recherche"}, {"search_close", "Fermer la recherche"}, {"theme", "Thème de couleurs"}, {"theme_system", "Thème du système"}, {"theme_light", "Thème clair"}, {"theme_dark", "Thème sombre"}, {"diagram_open", "Ouvrir le diagramme"}, {"diagram_expand", "Agrandir le diagramme"}, {"diagram_close", "Fermer le diagramme"}, {"markdown_version", "Lire cette page en Markdown brut"}, {"llms_index", "Lire l’index de la documentation pour les agents"}}},
      {"es",
       {{"skip_to_content", "Saltar al contenido"}, {"menu", "Menú"}, {"home", "Inicio"}, {"on_this_page", "En esta página"},
        {"previous", "Anterior"}, {"next", "Siguiente"}, {"previous_next", "Página anterior y siguiente"},
        {"search", "Buscar"}, {"search_label", "Buscar en este sitio"}, {"site_index", "Índice del sitio"},
        {"other_pages", "Otras páginas"}, {"built_with", "Creado con {generator}"}, {"sections", "Secciones"},
        {"pages", "Páginas"}, {"breadcrumb", "Ruta de navegación"}, {"links", "Enlaces"}, {"sites", "Sitios"},
        {"documentation_sites", "Sitios de documentación"}, {"languages", "Idiomas"},
        {"not_translated", "Esta página aún no se ha traducido ({language}); se muestra en su idioma original ({default})."},
        {"tags", "Etiquetas"}, {"tagged", "Etiqueta «{tag}»"}, {"page_count", "{count} páginas"},
        {"page_count_one", "1 página"}, {"copy_code", "Copiar código"}, {"copied", "Copiado"},
        {"moved", "Esta página se ha trasladado a"}, {"note", "Nota"}, {"tip", "Consejo"}, {"important", "Importante"},
        {"warning", "Advertencia"}, {"caution", "Precaución"}, {"info", "Información"}, {"danger", "Peligro"},
        {"details", "Detalles"}, {"footnotes", "Notas al pie"}, {"back_to_reference", "Volver a la referencia"}, {"overview", "Descripción general"}, {"features", "Características"}, {"search_open", "Abrir la búsqueda"}, {"search_close", "Cerrar la búsqueda"}, {"theme", "Tema de color"}, {"theme_system", "Tema del sistema"}, {"theme_light", "Tema claro"}, {"theme_dark", "Tema oscuro"}, {"diagram_open", "Abrir el diagrama"}, {"diagram_expand", "Ampliar el diagrama"}, {"diagram_close", "Cerrar el diagrama"}, {"markdown_version", "Leer esta página como Markdown sin formato"}, {"llms_index", "Leer el índice de la documentación para agentes"}}},
      {"it",
       {{"skip_to_content", "Vai al contenuto"}, {"menu", "Menu"}, {"home", "Pagina iniziale"}, {"on_this_page", "In questa pagina"},
        {"previous", "Precedente"}, {"next", "Successiva"}, {"previous_next", "Pagina precedente e successiva"},
        {"search", "Cerca"}, {"search_label", "Cerca in questo sito"}, {"site_index", "Indice del sito"},
        {"other_pages", "Altre pagine"}, {"built_with", "Realizzato con {generator}"}, {"sections", "Sezioni"},
        {"pages", "Pagine"}, {"breadcrumb", "Percorso di navigazione"}, {"links", "Link"}, {"sites", "Siti"},
        {"documentation_sites", "Siti di documentazione"}, {"languages", "Lingue"},
        {"not_translated", "Questa pagina non è ancora stata tradotta ({language}); viene mostrata nella lingua originale ({default})."},
        {"tags", "Tag"}, {"tagged", "Tag «{tag}»"}, {"page_count", "{count} pagine"}, {"page_count_one", "1 pagina"},
        {"copy_code", "Copia codice"}, {"copied", "Copiato"}, {"moved", "Questa pagina è stata spostata in"},
        {"note", "Nota"}, {"tip", "Suggerimento"}, {"important", "Importante"}, {"warning", "Avviso"},
        {"caution", "Attenzione"}, {"info", "Info"}, {"danger", "Pericolo"}, {"details", "Dettagli"},
        {"footnotes", "Note a piè di pagina"}, {"back_to_reference", "Torna al riferimento"}, {"overview", "Panoramica"}, {"features", "Funzionalità"}, {"search_open", "Apri la ricerca"}, {"search_close", "Chiudi la ricerca"}, {"theme", "Tema dei colori"}, {"theme_system", "Tema di sistema"}, {"theme_light", "Tema chiaro"}, {"theme_dark", "Tema scuro"}, {"diagram_open", "Apri il diagramma"}, {"diagram_expand", "Ingrandisci il diagramma"}, {"diagram_close", "Chiudi il diagramma"}, {"markdown_version", "Leggi questa pagina in Markdown semplice"}, {"llms_index", "Leggi l’indice della documentazione per gli agenti"}}},
      {"pt",
       {{"skip_to_content", "Pular para o conteúdo"}, {"menu", "Menu"}, {"home", "Início"}, {"on_this_page", "Nesta página"},
        {"previous", "Anterior"}, {"next", "Próxima"}, {"previous_next", "Página anterior e próxima"},
        {"search", "Pesquisar"}, {"search_label", "Pesquisar neste site"}, {"site_index", "Índice do site"},
        {"other_pages", "Outras páginas"}, {"built_with", "Criado com {generator}"}, {"sections", "Seções"},
        {"pages", "Páginas"}, {"breadcrumb", "Trilha de navegação"}, {"links", "Links"}, {"sites", "Sites"},
        {"documentation_sites", "Sites de documentação"}, {"languages", "Idiomas"},
        {"not_translated", "Esta página ainda não foi traduzida ({language}); ela é exibida no idioma original ({default})."},
        {"tags", "Etiquetas"}, {"tagged", "Etiqueta “{tag}”"}, {"page_count", "{count} páginas"},
        {"page_count_one", "1 página"}, {"copy_code", "Copiar código"}, {"copied", "Copiado"},
        {"moved", "Esta página foi movida para"}, {"note", "Nota"}, {"tip", "Dica"}, {"important", "Importante"},
        {"warning", "Aviso"}, {"caution", "Cuidado"}, {"info", "Informação"}, {"danger", "Perigo"},
        {"details", "Detalhes"}, {"footnotes", "Notas de rodapé"}, {"back_to_reference", "Voltar à referência"}, {"overview", "Visão geral"}, {"features", "Recursos"}, {"search_open", "Abrir a pesquisa"}, {"search_close", "Fechar a pesquisa"}, {"theme", "Tema de cores"}, {"theme_system", "Tema do sistema"}, {"theme_light", "Tema claro"}, {"theme_dark", "Tema escuro"}, {"diagram_open", "Abrir o diagrama"}, {"diagram_expand", "Ampliar o diagrama"}, {"diagram_close", "Fechar o diagrama"}, {"markdown_version", "Ler esta página em Markdown simples"}, {"llms_index", "Ler o índice da documentação para agentes"}}},
      {"nl",
       {{"skip_to_content", "Naar de inhoud"}, {"menu", "Menu"}, {"home", "Start"}, {"on_this_page", "Op deze pagina"},
        {"previous", "Vorige"}, {"next", "Volgende"}, {"previous_next", "Vorige en volgende pagina"}, {"search", "Zoeken"},
        {"search_label", "Doorzoek deze site"}, {"site_index", "Site-index"}, {"other_pages", "Andere pagina’s"},
        {"built_with", "Gemaakt met {generator}"}, {"sections", "Secties"}, {"pages", "Pagina’s"},
        {"breadcrumb", "Kruimelpad"}, {"links", "Links"}, {"sites", "Sites"}, {"documentation_sites", "Documentatiesites"},
        {"languages", "Talen"},
        {"not_translated", "Deze pagina is nog niet vertaald ({language}); ze wordt in de oorspronkelijke taal getoond ({default})."},
        {"tags", "Tags"}, {"tagged", "Tag ‘{tag}’"}, {"page_count", "{count} pagina’s"},
        {"page_count_one", "1 pagina"}, {"copy_code", "Code kopiëren"}, {"copied", "Gekopieerd"},
        {"moved", "Deze pagina is verplaatst naar"}, {"note", "Opmerking"}, {"tip", "Tip"}, {"important", "Belangrijk"},
        {"warning", "Waarschuwing"}, {"caution", "Let op"}, {"info", "Info"}, {"danger", "Gevaar"},
        {"details", "Details"}, {"footnotes", "Voetnoten"}, {"back_to_reference", "Terug naar de verwijzing"}, {"overview", "Overzicht"}, {"features", "Functies"}, {"search_open", "Zoeken openen"}, {"search_close", "Zoeken sluiten"}, {"theme", "Kleurenthema"}, {"theme_system", "Systeemthema"}, {"theme_light", "Licht thema"}, {"theme_dark", "Donker thema"}, {"diagram_open", "Diagram openen"}, {"diagram_expand", "Diagram vergroten"}, {"diagram_close", "Diagram sluiten"}, {"markdown_version", "Deze pagina lezen als gewone Markdown"}, {"llms_index", "De documentatie-index voor agents lezen"}}},
      {"pl",
       {{"skip_to_content", "Przejdź do treści"}, {"menu", "Menu"}, {"home", "Strona główna"}, {"on_this_page", "Na tej stronie"},
        {"previous", "Poprzednia"}, {"next", "Następna"}, {"previous_next", "Poprzednia i następna strona"},
        {"search", "Szukaj"}, {"search_label", "Przeszukaj tę witrynę"}, {"site_index", "Indeks witryny"},
        {"other_pages", "Inne strony"}, {"built_with", "Utworzono za pomocą {generator}"}, {"sections", "Sekcje"},
        {"pages", "Strony"}, {"breadcrumb", "Ścieżka nawigacji"}, {"links", "Linki"}, {"sites", "Witryny"},
        {"documentation_sites", "Witryny z dokumentacją"}, {"languages", "Języki"},
        {"not_translated", "Ta strona nie została jeszcze przetłumaczona ({language}); jest wyświetlana w języku oryginału ({default})."},
        {"tags", "Tagi"}, {"tagged", "Tag „{tag}”"}, {"page_count", "Liczba stron: {count}"},
        {"page_count_one", "1 strona"}, {"copy_code", "Kopiuj kod"}, {"copied", "Skopiowano"},
        {"moved", "Ta strona została przeniesiona do"}, {"note", "Uwaga"}, {"tip", "Wskazówka"}, {"important", "Ważne"},
        {"warning", "Ostrzeżenie"}, {"caution", "Przestroga"}, {"info", "Informacja"}, {"danger", "Niebezpieczeństwo"},
        {"details", "Szczegóły"}, {"footnotes", "Przypisy"}, {"back_to_reference", "Powrót do odwołania"}, {"overview", "Przegląd"}, {"features", "Funkcje"}, {"search_open", "Otwórz wyszukiwanie"}, {"search_close", "Zamknij wyszukiwanie"}, {"theme", "Motyw kolorów"}, {"theme_system", "Motyw systemowy"}, {"theme_light", "Jasny motyw"}, {"theme_dark", "Ciemny motyw"}, {"diagram_open", "Otwórz diagram"}, {"diagram_expand", "Powiększ diagram"}, {"diagram_close", "Zamknij diagram"}, {"markdown_version", "Czytaj tę stronę jako zwykły Markdown"}, {"llms_index", "Czytaj indeks dokumentacji dla agentów"}}},
      {"sv",
       {{"skip_to_content", "Hoppa till innehållet"}, {"menu", "Meny"}, {"home", "Start"}, {"on_this_page", "På den här sidan"},
        {"previous", "Föregående"}, {"next", "Nästa"}, {"previous_next", "Föregående och nästa sida"}, {"search", "Sök"},
        {"search_label", "Sök på webbplatsen"}, {"site_index", "Webbplatsindex"}, {"other_pages", "Andra sidor"},
        {"built_with", "Skapad med {generator}"}, {"sections", "Avsnitt"}, {"pages", "Sidor"}, {"breadcrumb", "Brödsmulor"},
        {"links", "Länkar"}, {"sites", "Webbplatser"}, {"documentation_sites", "Dokumentationswebbplatser"},
        {"languages", "Språk"},
        {"not_translated", "Den här sidan har inte översatts ännu ({language}); den visas på originalspråket ({default})."},
        {"tags", "Taggar"}, {"tagged", "Taggad ”{tag}”"}, {"page_count", "{count} sidor"},
        {"page_count_one", "1 sida"}, {"copy_code", "Kopiera kod"}, {"copied", "Kopierat"},
        {"moved", "Den här sidan har flyttats till"}, {"note", "Obs"}, {"tip", "Tips"}, {"important", "Viktigt"},
        {"warning", "Varning"}, {"caution", "Försiktighet"}, {"info", "Info"}, {"danger", "Fara"}, {"details", "Detaljer"},
        {"footnotes", "Fotnoter"}, {"back_to_reference", "Tillbaka till hänvisningen"}, {"overview", "Översikt"}, {"features", "Funktioner"}, {"search_open", "Öppna sökningen"}, {"search_close", "Stäng sökningen"}, {"theme", "Färgtema"}, {"theme_system", "Systemets tema"}, {"theme_light", "Ljust tema"}, {"theme_dark", "Mörkt tema"}, {"diagram_open", "Öppna diagrammet"}, {"diagram_expand", "Förstora diagrammet"}, {"diagram_close", "Stäng diagrammet"}, {"markdown_version", "Läs sidan som ren Markdown"}, {"llms_index", "Läs dokumentationsindexet för agenter"}}},
      {"da",
       {{"skip_to_content", "Spring til indholdet"}, {"menu", "Menu"}, {"home", "Forside"}, {"on_this_page", "På denne side"},
        {"previous", "Forrige"}, {"next", "Næste"}, {"previous_next", "Forrige og næste side"}, {"search", "Søg"},
        {"search_label", "Søg på webstedet"}, {"site_index", "Webstedsindeks"}, {"other_pages", "Andre sider"},
        {"built_with", "Lavet med {generator}"}, {"sections", "Afsnit"}, {"pages", "Sider"},
        {"breadcrumb", "Brødkrummesti"}, {"links", "Links"}, {"sites", "Websteder"},
        {"documentation_sites", "Dokumentationswebsteder"}, {"languages", "Sprog"},
        {"not_translated", "Denne side er endnu ikke oversat ({language}); den vises på originalsproget ({default})."},
        {"tags", "Tags"}, {"tagged", "Tag ”{tag}”"}, {"page_count", "{count} sider"}, {"page_count_one", "1 side"},
        {"copy_code", "Kopiér kode"}, {"copied", "Kopieret"}, {"moved", "Denne side er flyttet til"},
        {"note", "Bemærk"}, {"tip", "Tip"}, {"important", "Vigtigt"}, {"warning", "Advarsel"}, {"caution", "Forsigtig"},
        {"info", "Info"}, {"danger", "Fare"}, {"details", "Detaljer"}, {"footnotes", "Fodnoter"},
        {"back_to_reference", "Tilbage til henvisningen"}, {"overview", "Oversigt"}, {"features", "Funktioner"}, {"search_open", "Åbn søgning"}, {"search_close", "Luk søgning"}, {"theme", "Farvetema"}, {"theme_system", "Systemets tema"}, {"theme_light", "Lyst tema"}, {"theme_dark", "Mørkt tema"}, {"diagram_open", "Åbn diagrammet"}, {"diagram_expand", "Forstør diagrammet"}, {"diagram_close", "Luk diagrammet"}, {"markdown_version", "Læs siden som ren Markdown"}, {"llms_index", "Læs dokumentationsindekset til agenter"}}},
      {"nb",
       {{"skip_to_content", "Hopp til innholdet"}, {"menu", "Meny"}, {"home", "Forside"}, {"on_this_page", "På denne siden"},
        {"previous", "Forrige"}, {"next", "Neste"}, {"previous_next", "Forrige og neste side"}, {"search", "Søk"},
        {"search_label", "Søk på nettstedet"}, {"site_index", "Nettstedsindeks"}, {"other_pages", "Andre sider"},
        {"built_with", "Laget med {generator}"}, {"sections", "Seksjoner"}, {"pages", "Sider"},
        {"breadcrumb", "Brødsmulesti"}, {"links", "Lenker"}, {"sites", "Nettsteder"},
        {"documentation_sites", "Dokumentasjonsnettsteder"}, {"languages", "Språk"},
        {"not_translated", "Denne siden er ikke oversatt ennå ({language}); den vises på originalspråket ({default})."},
        {"tags", "Emneknagger"}, {"tagged", "Emneknagg «{tag}»"}, {"page_count", "{count} sider"},
        {"page_count_one", "1 side"}, {"copy_code", "Kopier kode"}, {"copied", "Kopiert"},
        {"moved", "Denne siden er flyttet til"}, {"note", "Merk"}, {"tip", "Tips"}, {"important", "Viktig"},
        {"warning", "Advarsel"}, {"caution", "Forsiktig"}, {"info", "Info"}, {"danger", "Fare"}, {"details", "Detaljer"},
        {"footnotes", "Fotnoter"}, {"back_to_reference", "Tilbake til henvisningen"}, {"overview", "Oversikt"}, {"features", "Funksjoner"}, {"search_open", "Åpne søket"}, {"search_close", "Lukk søket"}, {"theme", "Fargetema"}, {"theme_system", "Systemets tema"}, {"theme_light", "Lyst tema"}, {"theme_dark", "Mørkt tema"}, {"diagram_open", "Åpne diagrammet"}, {"diagram_expand", "Forstørr diagrammet"}, {"diagram_close", "Lukk diagrammet"}, {"markdown_version", "Les siden som ren Markdown"}, {"llms_index", "Les dokumentasjonsindeksen for agenter"}}},
      {"fi",
       {{"skip_to_content", "Siirry sisältöön"}, {"menu", "Valikko"}, {"home", "Etusivu"}, {"on_this_page", "Tällä sivulla"},
        {"previous", "Edellinen"}, {"next", "Seuraava"}, {"previous_next", "Edellinen ja seuraava sivu"},
        {"search", "Hae"}, {"search_label", "Hae sivustolta"}, {"site_index", "Sivustohakemisto"},
        {"other_pages", "Muut sivut"}, {"built_with", "Tehty työkalulla {generator}"}, {"sections", "Osiot"},
        {"pages", "Sivut"}, {"breadcrumb", "Murupolku"}, {"links", "Linkit"}, {"sites", "Sivustot"},
        {"documentation_sites", "Dokumentaatiosivustot"}, {"languages", "Kielet"},
        {"not_translated", "Tätä sivua ei ole vielä käännetty ({language}); se näytetään alkuperäisellä kielellä ({default})."},
        {"tags", "Tunnisteet"}, {"tagged", "Tunniste ”{tag}”"}, {"page_count", "{count} sivua"},
        {"page_count_one", "1 sivu"}, {"copy_code", "Kopioi koodi"}, {"copied", "Kopioitu"},
        {"moved", "Tämä sivu on siirretty osoitteeseen"}, {"note", "Huomautus"}, {"tip", "Vinkki"},
        {"important", "Tärkeää"}, {"warning", "Varoitus"}, {"caution", "Varo"}, {"info", "Tietoa"}, {"danger", "Vaara"},
        {"details", "Lisätiedot"}, {"footnotes", "Alaviitteet"}, {"back_to_reference", "Takaisin viittaukseen"}, {"overview", "Yleiskatsaus"}, {"features", "Ominaisuudet"}, {"search_open", "Avaa haku"}, {"search_close", "Sulje haku"}, {"theme", "Väriteema"}, {"theme_system", "Järjestelmän teema"}, {"theme_light", "Vaalea teema"}, {"theme_dark", "Tumma teema"}, {"diagram_open", "Avaa kaavio"}, {"diagram_expand", "Suurenna kaavio"}, {"diagram_close", "Sulje kaavio"}, {"markdown_version", "Lue tämä sivu pelkkänä Markdownina"}, {"llms_index", "Lue agenteille tarkoitettu dokumentaation hakemisto"}}},
      {"cs",
       {{"skip_to_content", "Přejít na obsah"}, {"menu", "Nabídka"}, {"home", "Domů"}, {"on_this_page", "Na této stránce"},
        {"previous", "Předchozí"}, {"next", "Další"}, {"previous_next", "Předchozí a další stránka"}, {"search", "Hledat"},
        {"search_label", "Prohledat tento web"}, {"site_index", "Rejstřík webu"}, {"other_pages", "Další stránky"},
        {"built_with", "Vytvořeno pomocí {generator}"}, {"sections", "Oddíly"}, {"pages", "Stránky"},
        {"breadcrumb", "Drobečková navigace"}, {"links", "Odkazy"}, {"sites", "Weby"},
        {"documentation_sites", "Weby s dokumentací"}, {"languages", "Jazyky"},
        {"not_translated", "Tato stránka zatím nebyla přeložena ({language}); zobrazuje se v původním jazyce ({default})."},
        {"tags", "Štítky"}, {"tagged", "Štítek „{tag}“"}, {"page_count", "Počet stránek: {count}"},
        {"page_count_one", "1 stránka"}, {"copy_code", "Kopírovat kód"}, {"copied", "Zkopírováno"},
        {"moved", "Tato stránka byla přesunuta na"}, {"note", "Poznámka"}, {"tip", "Tip"}, {"important", "Důležité"},
        {"warning", "Upozornění"}, {"caution", "Pozor"}, {"info", "Informace"}, {"danger", "Nebezpečí"},
        {"details", "Podrobnosti"}, {"footnotes", "Poznámky pod čarou"}, {"back_to_reference", "Zpět na odkaz"}, {"overview", "Přehled"}, {"features", "Funkce"}, {"search_open", "Otevřít hledání"}, {"search_close", "Zavřít hledání"}, {"theme", "Barevný motiv"}, {"theme_system", "Motiv systému"}, {"theme_light", "Světlý motiv"}, {"theme_dark", "Tmavý motiv"}, {"diagram_open", "Otevřít diagram"}, {"diagram_expand", "Zvětšit diagram"}, {"diagram_close", "Zavřít diagram"}, {"markdown_version", "Číst tuto stránku jako prostý Markdown"}, {"llms_index", "Číst rejstřík dokumentace pro agenty"}}},
      {"ru",
       {{"skip_to_content", "Перейти к содержимому"}, {"menu", "Меню"}, {"home", "Главная"}, {"on_this_page", "На этой странице"},
        {"previous", "Назад"}, {"next", "Далее"}, {"previous_next", "Предыдущая и следующая страница"},
        {"search", "Поиск"}, {"search_label", "Поиск по сайту"}, {"site_index", "Указатель сайта"},
        {"other_pages", "Другие страницы"}, {"built_with", "Создано с помощью {generator}"}, {"sections", "Разделы"},
        {"pages", "Страницы"}, {"breadcrumb", "Навигационная цепочка"}, {"links", "Ссылки"}, {"sites", "Сайты"},
        {"documentation_sites", "Сайты документации"}, {"languages", "Языки"},
        {"not_translated", "Эта страница ещё не переведена ({language}); она показана на языке оригинала ({default})."},
        {"tags", "Теги"}, {"tagged", "Тег «{tag}»"}, {"page_count", "Страниц: {count}"},
        {"page_count_one", "1 страница"}, {"copy_code", "Копировать код"}, {"copied", "Скопировано"},
        {"moved", "Эта страница перемещена на"}, {"note", "Примечание"}, {"tip", "Совет"}, {"important", "Важно"},
        {"warning", "Предупреждение"}, {"caution", "Осторожно"}, {"info", "Информация"}, {"danger", "Опасно"},
        {"details", "Подробности"}, {"footnotes", "Сноски"}, {"back_to_reference", "Вернуться к ссылке"}, {"overview", "Обзор"}, {"features", "Возможности"}, {"search_open", "Открыть поиск"}, {"search_close", "Закрыть поиск"}, {"theme", "Цветовая тема"}, {"theme_system", "Системная тема"}, {"theme_light", "Светлая тема"}, {"theme_dark", "Тёмная тема"}, {"diagram_open", "Открыть диаграмму"}, {"diagram_expand", "Увеличить диаграмму"}, {"diagram_close", "Закрыть диаграмму"}, {"markdown_version", "Читать эту страницу как Markdown"}, {"llms_index", "Читать указатель документации для агентов"}}},
      {"uk",
       {{"skip_to_content", "Перейти до вмісту"}, {"menu", "Меню"}, {"home", "Головна"}, {"on_this_page", "На цій сторінці"},
        {"previous", "Назад"}, {"next", "Далі"}, {"previous_next", "Попередня й наступна сторінка"},
        {"search", "Пошук"}, {"search_label", "Пошук на сайті"}, {"site_index", "Покажчик сайту"},
        {"other_pages", "Інші сторінки"}, {"built_with", "Створено за допомогою {generator}"}, {"sections", "Розділи"},
        {"pages", "Сторінки"}, {"breadcrumb", "Навігаційний ланцюжок"}, {"links", "Посилання"}, {"sites", "Сайти"},
        {"documentation_sites", "Сайти документації"}, {"languages", "Мови"},
        {"not_translated", "Цю сторінку ще не перекладено ({language}); її показано мовою оригіналу ({default})."},
        {"tags", "Теги"}, {"tagged", "Тег «{tag}»"}, {"page_count", "Сторінок: {count}"},
        {"page_count_one", "1 сторінка"}, {"copy_code", "Копіювати код"}, {"copied", "Скопійовано"},
        {"moved", "Цю сторінку переміщено на"}, {"note", "Примітка"}, {"tip", "Порада"}, {"important", "Важливо"},
        {"warning", "Попередження"}, {"caution", "Обережно"}, {"info", "Інформація"}, {"danger", "Небезпека"},
        {"details", "Подробиці"}, {"footnotes", "Виноски"}, {"back_to_reference", "Повернутися до посилання"}, {"overview", "Огляд"}, {"features", "Можливості"}, {"search_open", "Відкрити пошук"}, {"search_close", "Закрити пошук"}, {"theme", "Колірна тема"}, {"theme_system", "Системна тема"}, {"theme_light", "Світла тема"}, {"theme_dark", "Темна тема"}, {"diagram_open", "Відкрити діаграму"}, {"diagram_expand", "Збільшити діаграму"}, {"diagram_close", "Закрити діаграму"}, {"markdown_version", "Читати цю сторінку як Markdown"}, {"llms_index", "Читати покажчик документації для агентів"}}},
      {"tr",
       {{"skip_to_content", "İçeriğe geç"}, {"menu", "Menü"}, {"home", "Ana sayfa"}, {"on_this_page", "Bu sayfada"}, {"previous", "Önceki"},
        {"next", "Sonraki"}, {"previous_next", "Önceki ve sonraki sayfa"}, {"search", "Ara"},
        {"search_label", "Bu sitede ara"}, {"site_index", "Site dizini"}, {"other_pages", "Diğer sayfalar"},
        {"built_with", "{generator} ile oluşturuldu"}, {"sections", "Bölümler"}, {"pages", "Sayfalar"},
        {"breadcrumb", "İçerik yolu"}, {"links", "Bağlantılar"}, {"sites", "Siteler"},
        {"documentation_sites", "Dokümantasyon siteleri"}, {"languages", "Diller"},
        {"not_translated", "Bu sayfa henüz çevrilmedi ({language}); özgün dilinde gösteriliyor ({default})."},
        {"tags", "Etiketler"}, {"tagged", "“{tag}” etiketi"}, {"page_count", "{count} sayfa"},
        {"page_count_one", "1 sayfa"}, {"copy_code", "Kodu kopyala"}, {"copied", "Kopyalandı"},
        {"moved", "Bu sayfa şuraya taşındı:"}, {"note", "Not"}, {"tip", "İpucu"}, {"important", "Önemli"},
        {"warning", "Uyarı"}, {"caution", "Dikkat"}, {"info", "Bilgi"}, {"danger", "Tehlike"}, {"details", "Ayrıntılar"},
        {"footnotes", "Dipnotlar"}, {"back_to_reference", "Atfa dön"}, {"overview", "Genel bakış"}, {"features", "Özellikler"}, {"search_open", "Aramayı aç"}, {"search_close", "Aramayı kapat"}, {"theme", "Renk teması"}, {"theme_system", "Sistem teması"}, {"theme_light", "Açık tema"}, {"theme_dark", "Koyu tema"}, {"diagram_open", "Diyagramı aç"}, {"diagram_expand", "Diyagramı büyüt"}, {"diagram_close", "Diyagramı kapat"}, {"markdown_version", "Bu sayfayı düz Markdown olarak oku"}, {"llms_index", "Ajanlar için belge dizinini oku"}}},
      {"ja",
       {{"skip_to_content", "コンテンツへスキップ"}, {"menu", "メニュー"}, {"home", "ホーム"}, {"on_this_page", "このページの内容"},
        {"previous", "前へ"}, {"next", "次へ"}, {"previous_next", "前のページと次のページ"}, {"search", "検索"},
        {"search_label", "このサイトを検索"}, {"site_index", "サイト索引"}, {"other_pages", "その他のページ"},
        {"built_with", "{generator} で作成"}, {"sections", "セクション"}, {"pages", "ページ"},
        {"breadcrumb", "パンくずリスト"}, {"links", "リンク"}, {"sites", "サイト"},
        {"documentation_sites", "ドキュメントサイト"}, {"languages", "言語"},
        {"not_translated", "このページはまだ翻訳されていません（{language}）。原文（{default}）で表示しています。"},
        {"tags", "タグ"}, {"tagged", "タグ「{tag}」"}, {"page_count", "{count} ページ"}, {"page_count_one", "1 ページ"},
        {"copy_code", "コードをコピー"}, {"copied", "コピーしました"}, {"moved", "このページの移動先:"},
        {"note", "注記"}, {"tip", "ヒント"}, {"important", "重要"}, {"warning", "警告"}, {"caution", "注意"},
        {"info", "情報"}, {"danger", "危険"}, {"details", "詳細"}, {"footnotes", "脚注"},
        {"back_to_reference", "参照元に戻る"}, {"overview", "概要"}, {"features", "特長"}, {"search_open", "検索を開く"}, {"search_close", "検索を閉じる"}, {"theme", "カラーテーマ"}, {"theme_system", "システムのテーマ"}, {"theme_light", "ライトテーマ"}, {"theme_dark", "ダークテーマ"}, {"diagram_open", "図を開く"}, {"diagram_expand", "図を拡大"}, {"diagram_close", "図を閉じる"}, {"markdown_version", "このページを Markdown で読む"}, {"llms_index", "エージェント向けのドキュメント索引を読む"}}},
      {"zh",
       {{"skip_to_content", "跳到主要内容"}, {"menu", "菜单"}, {"home", "首页"}, {"on_this_page", "本页内容"}, {"previous", "上一页"},
        {"next", "下一页"}, {"previous_next", "上一页和下一页"}, {"search", "搜索"}, {"search_label", "搜索本站"},
        {"site_index", "站点索引"}, {"other_pages", "其他页面"}, {"built_with", "使用 {generator} 构建"},
        {"sections", "栏目"}, {"pages", "页面"}, {"breadcrumb", "面包屑导航"}, {"links", "链接"}, {"sites", "站点"},
        {"documentation_sites", "文档站点"}, {"languages", "语言"},
        {"not_translated", "此页面尚未翻译（{language}），以原文（{default}）显示。"}, {"tags", "标签"},
        {"tagged", "标签“{tag}”"}, {"page_count", "{count} 个页面"}, {"page_count_one", "1 个页面"},
        {"copy_code", "复制代码"}, {"copied", "已复制"}, {"moved", "此页面已移至"}, {"note", "备注"}, {"tip", "提示"},
        {"important", "重要"}, {"warning", "警告"}, {"caution", "注意"}, {"info", "信息"}, {"danger", "危险"},
        {"details", "详情"}, {"footnotes", "脚注"}, {"back_to_reference", "返回引用处"}, {"overview", "概览"}, {"features", "特性"}, {"search_open", "打开搜索"}, {"search_close", "关闭搜索"}, {"theme", "颜色主题"}, {"theme_system", "跟随系统"}, {"theme_light", "浅色主题"}, {"theme_dark", "深色主题"}, {"diagram_open", "打开图表"}, {"diagram_expand", "放大图表"}, {"diagram_close", "关闭图表"}, {"markdown_version", "以纯 Markdown 阅读此页"}, {"llms_index", "阅读面向智能体的文档索引"}}},
      {"zh-TW",
       {{"skip_to_content", "跳至主要內容"}, {"menu", "選單"}, {"home", "首頁"}, {"on_this_page", "本頁內容"}, {"previous", "上一頁"},
        {"next", "下一頁"}, {"previous_next", "上一頁與下一頁"}, {"search", "搜尋"}, {"search_label", "搜尋本站"},
        {"site_index", "網站索引"}, {"other_pages", "其他頁面"}, {"built_with", "使用 {generator} 建置"},
        {"sections", "區塊"}, {"pages", "頁面"}, {"breadcrumb", "導覽路徑"}, {"links", "連結"}, {"sites", "網站"},
        {"documentation_sites", "文件網站"}, {"languages", "語言"},
        {"not_translated", "此頁面尚未翻譯（{language}），以原文（{default}）顯示。"}, {"tags", "標籤"},
        {"tagged", "標籤「{tag}」"}, {"page_count", "{count} 個頁面"}, {"page_count_one", "1 個頁面"},
        {"copy_code", "複製程式碼"}, {"copied", "已複製"}, {"moved", "此頁面已移至"}, {"note", "備註"}, {"tip", "提示"},
        {"important", "重要"}, {"warning", "警告"}, {"caution", "注意"}, {"info", "資訊"}, {"danger", "危險"},
        {"details", "詳細資訊"}, {"footnotes", "註腳"}, {"back_to_reference", "返回引用處"}, {"overview", "概覽"}, {"features", "特色"}, {"search_open", "開啟搜尋"}, {"search_close", "關閉搜尋"}, {"theme", "色彩主題"}, {"theme_system", "跟隨系統"}, {"theme_light", "淺色主題"}, {"theme_dark", "深色主題"}, {"diagram_open", "開啟圖表"}, {"diagram_expand", "放大圖表"}, {"diagram_close", "關閉圖表"}, {"markdown_version", "以純 Markdown 閱讀此頁"}, {"llms_index", "閱讀供代理程式使用的文件索引"}}},
      {"ko",
       {{"skip_to_content", "본문으로 건너뛰기"}, {"menu", "메뉴"}, {"home", "홈"}, {"on_this_page", "이 페이지의 내용"},
        {"previous", "이전"}, {"next", "다음"}, {"previous_next", "이전 페이지와 다음 페이지"}, {"search", "검색"},
        {"search_label", "이 사이트 검색"}, {"site_index", "사이트 색인"}, {"other_pages", "기타 페이지"},
        {"built_with", "{generator}(으)로 제작"}, {"sections", "섹션"}, {"pages", "페이지"}, {"breadcrumb", "이동 경로"},
        {"links", "링크"}, {"sites", "사이트"}, {"documentation_sites", "문서 사이트"}, {"languages", "언어"},
        {"not_translated", "이 페이지는 아직 번역되지 않았습니다({language}). 원문({default})으로 표시됩니다."},
        {"tags", "태그"}, {"tagged", "태그 “{tag}”"}, {"page_count", "페이지 {count}개"},
        {"page_count_one", "페이지 1개"}, {"copy_code", "코드 복사"}, {"copied", "복사됨"},
        {"moved", "이 페이지는 다음 위치로 이동했습니다:"}, {"note", "참고"}, {"tip", "팁"}, {"important", "중요"},
        {"warning", "경고"}, {"caution", "주의"}, {"info", "정보"}, {"danger", "위험"}, {"details", "세부 정보"},
        {"footnotes", "각주"}, {"back_to_reference", "참조 위치로 돌아가기"}, {"overview", "개요"}, {"features", "기능"}, {"search_open", "검색 열기"}, {"search_close", "검색 닫기"}, {"theme", "색상 테마"}, {"theme_system", "시스템 테마"}, {"theme_light", "밝은 테마"}, {"theme_dark", "어두운 테마"}, {"diagram_open", "다이어그램 열기"}, {"diagram_expand", "다이어그램 확대"}, {"diagram_close", "다이어그램 닫기"}, {"markdown_version", "이 페이지를 Markdown으로 읽기"}, {"llms_index", "에이전트용 문서 색인 읽기"}}},
      {"ar",
       {{"skip_to_content", "تخطَّ إلى المحتوى"}, {"menu", "القائمة"}, {"home", "الرئيسية"}, {"on_this_page", "في هذه الصفحة"},
        {"previous", "السابق"}, {"next", "التالي"}, {"previous_next", "الصفحة السابقة والتالية"}, {"search", "بحث"},
        {"search_label", "ابحث في هذا الموقع"}, {"site_index", "فهرس الموقع"}, {"other_pages", "صفحات أخرى"},
        {"built_with", "أُنشئ باستخدام {generator}"}, {"sections", "الأقسام"}, {"pages", "الصفحات"},
        {"breadcrumb", "مسار التنقل"}, {"links", "روابط"}, {"sites", "المواقع"}, {"documentation_sites", "مواقع التوثيق"},
        {"languages", "اللغات"},
        {"not_translated", "لم تُترجم هذه الصفحة بعد ({language})؛ وهي معروضة بلغتها الأصلية ({default})."},
        {"tags", "الوسوم"}, {"tagged", "الوسم «{tag}»"}, {"page_count", "عدد الصفحات: {count}"},
        {"page_count_one", "صفحة واحدة"}, {"copy_code", "نسخ الشيفرة"}, {"copied", "تم النسخ"},
        {"moved", "نُقلت هذه الصفحة إلى"}, {"note", "ملاحظة"}, {"tip", "تلميح"}, {"important", "مهم"},
        {"warning", "تحذير"}, {"caution", "تنبيه"}, {"info", "معلومات"}, {"danger", "خطر"}, {"details", "التفاصيل"},
        {"footnotes", "الحواشي"}, {"back_to_reference", "العودة إلى الإحالة"}, {"overview", "نظرة عامة"}, {"features", "الميزات"}, {"search_open", "فتح البحث"}, {"search_close", "إغلاق البحث"}, {"theme", "سمة الألوان"}, {"theme_system", "سمة النظام"}, {"theme_light", "سمة فاتحة"}, {"theme_dark", "سمة داكنة"}, {"diagram_open", "فتح المخطط"}, {"diagram_expand", "تكبير المخطط"}, {"diagram_close", "إغلاق المخطط"}, {"markdown_version", "اقرأ هذه الصفحة بصيغة Markdown"}, {"llms_index", "اقرأ فهرس التوثيق المخصص للوكلاء"}}},
      {"he",
       {{"skip_to_content", "דלג לתוכן"}, {"menu", "תפריט"}, {"home", "דף הבית"}, {"on_this_page", "בדף זה"}, {"previous", "הקודם"},
        {"next", "הבא"}, {"previous_next", "הדף הקודם והבא"}, {"search", "חיפוש"}, {"search_label", "חיפוש באתר"},
        {"site_index", "מפתח האתר"}, {"other_pages", "דפים נוספים"}, {"built_with", "נבנה באמצעות {generator}"},
        {"sections", "מקטעים"}, {"pages", "דפים"}, {"breadcrumb", "פירורי לחם"}, {"links", "קישורים"},
        {"sites", "אתרים"}, {"documentation_sites", "אתרי תיעוד"}, {"languages", "שפות"},
        {"not_translated", "דף זה עדיין לא תורגם ({language}); הוא מוצג בשפת המקור ({default})."}, {"tags", "תגיות"},
        {"tagged", "תגית: {tag}"}, {"page_count", "מספר דפים: {count}"}, {"page_count_one", "דף אחד"},
        {"copy_code", "העתקת קוד"}, {"copied", "הועתק"}, {"moved", "דף זה הועבר אל"}, {"note", "הערה"},
        {"tip", "עצה"}, {"important", "חשוב"}, {"warning", "אזהרה"}, {"caution", "זהירות"}, {"info", "מידע"},
        {"danger", "סכנה"}, {"details", "פרטים"}, {"footnotes", "הערות שוליים"}, {"back_to_reference", "חזרה להפניה"}, {"overview", "סקירה כללית"}, {"features", "תכונות"}, {"search_open", "פתיחת החיפוש"}, {"search_close", "סגירת החיפוש"}, {"theme", "ערכת צבעים"}, {"theme_system", "ערכת המערכת"}, {"theme_light", "ערכה בהירה"}, {"theme_dark", "ערכה כהה"}, {"diagram_open", "פתיחת התרשים"}, {"diagram_expand", "הגדלת התרשים"}, {"diagram_close", "סגירת התרשים"}, {"markdown_version", "קריאת הדף כ-Markdown פשוט"}, {"llms_index", "קריאת אינדקס התיעוד לסוכנים"}}},
  };
  return kTranslations;
}

std::string lowerAscii(std::string_view value) {
  std::string result(value);
  for (char& byte : result) byte = static_cast<char>(std::tolower(static_cast<unsigned char>(byte)));
  return result;
}

// The tag's first subtag ("de" for "de-CH").
std::string primaryLanguage(std::string_view lang) { return lowerAscii(lang.substr(0, lang.find('-'))); }

// Chinese in traditional script: zh-TW, zh-HK, zh-MO, or any zh-Hant tag.
bool traditionalChinese(std::string_view lang) {
  const auto lowered = lowerAscii(lang);
  return lowered.starts_with("zh-") &&
         (lowered.find("-hant") != std::string::npos || lowered.ends_with("-tw") || lowered.ends_with("-hk") ||
          lowered.ends_with("-mo"));
}

const Translation* translationFor(std::string_view lang) {
  std::string wanted = lowerAscii(lang);
  if (traditionalChinese(lang)) wanted = "zh-tw";
  for (const auto& translation : translations()) {
    if (lowerAscii(translation.lang) == wanted) return &translation;
  }
  auto primary = primaryLanguage(lang);
  if (primary == "no" || primary == "nn") primary = "nb";  // Norwegian: the Bokmål set reads well to every reader
  for (const auto& translation : translations()) {
    if (translation.lang == primary) return &translation;
  }
  return nullptr;
}

struct LanguageName {
  std::string_view lang;
  std::string_view name;
};

// Each language's name in itself, for a language menu without labels.
constexpr LanguageName kLanguageNames[] = {
    {"ar", "العربية"},        {"bg", "Български"},      {"ca", "Català"},      {"cs", "Čeština"},
    {"da", "Dansk"},          {"de", "Deutsch"},        {"el", "Ελληνικά"},    {"en", "English"},
    {"es", "Español"},        {"et", "Eesti"},          {"eu", "Euskara"},     {"fa", "فارسی"},
    {"fi", "Suomi"},          {"fr", "Français"},       {"ga", "Gaeilge"},     {"gl", "Galego"},
    {"he", "עברית"},          {"hi", "हिन्दी"},           {"hr", "Hrvatski"},    {"hu", "Magyar"},
    {"id", "Bahasa Indonesia"}, {"is", "Íslenska"},     {"it", "Italiano"},    {"ja", "日本語"},
    {"ko", "한국어"},           {"lt", "Lietuvių"},       {"lv", "Latviešu"},    {"ms", "Bahasa Melayu"},
    {"nb", "Norsk bokmål"},   {"nl", "Nederlands"},     {"nn", "Norsk nynorsk"}, {"no", "Norsk"},
    {"pl", "Polski"},         {"pt", "Português"},      {"pt-br", "Português (Brasil)"}, {"pt-pt", "Português (Portugal)"},
    {"ro", "Română"},         {"ru", "Русский"},        {"sk", "Slovenčina"},  {"sl", "Slovenščina"},
    {"sr", "Српски"},         {"sv", "Svenska"},        {"th", "ไทย"},          {"tr", "Türkçe"},
    {"uk", "Українська"},     {"ur", "اردو"},           {"vi", "Tiếng Việt"},  {"zh", "中文"},
    {"zh-cn", "简体中文"},      {"zh-hans", "简体中文"},    {"zh-hant", "繁體中文"}, {"zh-hk", "繁體中文（香港）"},
    {"zh-tw", "繁體中文"},
};

}  // namespace

const std::vector<std::string_view>& docsUiKeys() {
  static const std::vector<std::string_view> kKeys = [] {
    std::vector<std::string_view> keys;
    for (const auto& [key, field] : fields()) keys.push_back(key);
    return keys;
  }();
  return kKeys;
}

bool setDocsUiText(DocsUiText& text, std::string_view key, std::string value) {
  for (const auto& [name, field] : fields()) {
    if (name == key) {
      text.*field = std::move(value);
      return true;
    }
  }
  return false;
}

DocsUiText docsUiTextFor(std::string_view lang) {
  DocsUiText text;
  if (const auto* translation = translationFor(lang)) {
    for (const auto& [key, value] : translation->texts) setDocsUiText(text, key, std::string(value));
  }
  return text;
}

std::vector<std::string_view> docsUiLanguages() {
  std::vector<std::string_view> languages{"en"};
  for (const auto& translation : translations()) languages.push_back(translation.lang);
  return languages;
}

std::string docsLanguageName(std::string_view lang) {
  const auto lowered = lowerAscii(lang);
  for (const auto& entry : kLanguageNames) {
    if (entry.lang == lowered) return std::string(entry.name);
  }
  if (traditionalChinese(lang)) return "繁體中文";
  const auto primary = primaryLanguage(lang);
  for (const auto& entry : kLanguageNames) {
    if (entry.lang == primary) return std::string(entry.name);
  }
  return std::string(lang);
}

bool docsLanguageIsRightToLeft(std::string_view lang) {
  const auto primary = primaryLanguage(lang);
  for (const auto* rtl : {"ar", "he", "fa", "ur", "ps", "sd", "yi", "dv", "ug", "ckb"}) {
    if (primary == rtl) return true;
  }
  return false;
}

bool isValidDocsLanguageTag(std::string_view lang) {
  if (lang.empty() || lang.size() > 35) return false;
  std::size_t index = 0;
  for (std::size_t start = 0; start <= lang.size(); ++index) {
    const auto dash = lang.find('-', start);
    const auto part = lang.substr(start, dash == std::string_view::npos ? std::string_view::npos : dash - start);
    const bool letters = std::all_of(part.begin(), part.end(), [](unsigned char byte) { return std::isalpha(byte) != 0; });
    const bool alphanumeric = std::all_of(part.begin(), part.end(), [](unsigned char byte) { return std::isalnum(byte) != 0; });
    if (index == 0 ? (!letters || part.size() < 2 || part.size() > 3) : (!alphanumeric || part.size() < 2 || part.size() > 8)) {
      return false;
    }
    if (dash == std::string_view::npos) break;
    start = dash + 1;
  }
  return true;
}

std::string fillDocsUiText(std::string_view text, const std::map<std::string, std::string>& values) {
  std::string result;
  for (std::size_t index = 0; index < text.size();) {
    if (text[index] == '{') {
      const auto close = text.find('}', index + 1);
      if (close != std::string_view::npos) {
        const auto found = values.find(std::string(text.substr(index + 1, close - index - 1)));
        if (found != values.end()) {
          result += found->second;
          index = close + 1;
          continue;
        }
      }
    }
    result += text[index++];
  }
  return result;
}

}  // namespace ckgit
