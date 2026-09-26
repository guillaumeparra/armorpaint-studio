// Script ArmorPaint (--script) du patch obj-float-exponent : pour chaque objet importé, nom, nombre d'indices,
// échelle de packing (scale_pos = plus grande coordonnée en valeur absolue) et premier sommet packé (int16).
// ArmorPaint --background <fichier.obj> --script obj_scale.c
char *describe(mesh_object_t *mo, char *name) {
	mesh_data_t *d = mo->data;
	any_array_t *vas = d->vertex_arrays;
	vertex_array_t *va = vas->buffer[0];
	i16_array_t *p = va->values;
	return string(" %s(idx=%d scale=%f v0=%d,%d,%d)", name, d->index_array->length, d->scale_pos, p->buffer[0], p->buffer[1], p->buffer[2]);
}

int main() {
	context_t *c = script_get_context();
	mesh_object_t *m = c->paint_object;
	any_array_t *ch = m->base->children;
	char *s = describe(m, m->base->name);
	for (int i = 0; i < ch->length; ++i) {
		object_t *o = ch->buffer[i];
		s = string("%s%s", s, describe(o->ext, o->name));
	}
	console_log(s);
	return 0;
}
