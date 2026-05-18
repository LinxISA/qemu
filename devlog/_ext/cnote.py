from docutils import nodes
from docutils.parsers.rst import Directive
from docutils.parsers.rst import directives

class cnote_node(nodes.Admonition, nodes.Element):
    pass

def visit_cnote_node(self, node):
    self.body.append(self.starttag(node, 'div', CLASS=('admonition note')))
    self.set_first_last(node)

def depart_cnote_node(self, node):
    self.depart_admonition(node)

class cnote_directive(Directive):

    has_content = True
    required_arguments = 0
    optional_arguments = 0
    option_spec = { 'title' : directives.unchanged }

    def run(self):
        node = cnote_node(rawsource=self.content)
        if 'title' in self.options.keys():
            title = self.options['title']
        else:
            title = 'Implementation Note'
        node += nodes.title(text=title)
        self.state.nested_parse(self.content, self.content_offset, node)

        return [node]

def setup(app):
    vd = visit_cnote_node, depart_cnote_node
    app.add_node(cnote_node, html=vd, latex=vd, text=vd)
    app.add_directive("cnote", cnote_directive)

    return {
        'version': '0.1',
        'parallel_read_safe': True,
        'parallel_write_safe': True,
    }
