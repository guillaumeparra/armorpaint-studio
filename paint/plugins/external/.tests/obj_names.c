// Script ArmorPaint (--script) du patch obj-global-indices : nom et nombre d'indices de chaque objet importé.
// ArmorPaint --background <fichier.obj> --script obj_names.c
int main() {
	context_t *c = script_get_context();
	mesh_object_t *m = c->paint_object;
	any_array_t *ch = m->base->children;
	char *s = string("root=%s idx=%d children=%d:", m->base->name, m->data->index_array->length, ch->length);
	for (int i = 0; i < ch->length; ++i) {
		object_t *o = ch->buffer[i];
		mesh_object_t *mo = o->ext;
		s = string("%s %s(idx=%d)", s, o->name, mo->data->index_array->length);
	}
	console_log(s);
	return 0;
}
